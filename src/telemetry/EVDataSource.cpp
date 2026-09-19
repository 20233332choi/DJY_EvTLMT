#include "EVDataSource.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using json = nlohmann::json;

constexpr float kGearRatio = 3.8f;
constexpr float kTireRadiusM = 0.2286f;
constexpr float kPi = 3.14159265358979323846f;

float SpeedFromMotorRpm(std::uint16_t left, std::uint16_t right) {
    const float motorRpm = 0.5f * (static_cast<float>(left) + static_cast<float>(right));
    const float wheelRpm = motorRpm / kGearRatio;
    return wheelRpm * (2.0f * kPi * kTireRadiusM) / 60.0f * 3.6f;
}

template <typename T>
T Number(const json& packet, const char* primary, const char* alias, T fallback) {
    try {
        if (packet.contains(primary) && packet[primary].is_number()) return packet[primary].get<T>();
        if (alias && packet.contains(alias) && packet[alias].is_number()) return packet[alias].get<T>();
    } catch (...) {
    }
    return fallback;
}

bool Boolean(const json& packet, const char* key, bool fallback) {
    try {
        if (!packet.contains(key)) return fallback;
        if (packet[key].is_boolean()) return packet[key].get<bool>();
        if (packet[key].is_number_integer()) return packet[key].get<int>() != 0;
    } catch (...) {
    }
    return fallback;
}

void Text(const json& packet, const char* key, char* destination, std::size_t capacity) {
    try {
        if (!packet.contains(key) || !packet[key].is_string() || capacity == 0) return;
        const std::string value = packet[key].get<std::string>();
        strncpy_s(destination, capacity, value.c_str(), _TRUNCATE);
    } catch (...) {
    }
}

}  // namespace

EVDataSource::EVDataSource(int port) : port_(port) {
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;
    wsaStarted_ = true;

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return;

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port_));
    address.sin_addr.s_addr = INADDR_ANY;
    if (bind(socket_, reinterpret_cast<SOCKADDR*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    DWORD timeoutMs = 100;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    running_ = true;
    listener_ = std::thread(&EVDataSource::ListenLoop, this);
}

EVDataSource::~EVDataSource() {
    running_ = false;
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (listener_.joinable()) listener_.join();
    if (wsaStarted_) WSACleanup();
}

void EVDataSource::ListenLoop() {
    char buffer[4096];
    while (running_.load()) {
        sockaddr_in sender = {};
        int senderLength = sizeof(sender);
        const int received = recvfrom(socket_, buffer, sizeof(buffer) - 1, 0,
                                      reinterpret_cast<SOCKADDR*>(&sender), &senderLength);
        if (received <= 0) continue;
        buffer[received] = '\0';
        ParsePacket(buffer, received);
    }
}

void EVDataSource::ParsePacket(const char* payload, int length) {
    try {
        const json packet = json::parse(payload, payload + length);
        if (!packet.is_object()) return;

        std::lock_guard<std::mutex> lock(mutex_);
        EVTelemetry next = pendingTelemetry_;
        next.sequence = Number<std::uint32_t>(packet, "seq", "sequence", next.sequence);
        next.timestampMs = Number<std::uint32_t>(packet, "timestamp_ms", "timestamp", next.timestampMs);
        next.vehicleOnline = Boolean(packet, "vehicle_online", Boolean(packet, "online", false));
        next.vehicleAgeMs = Number<float>(packet, "vehicle_age_ms", "age_ms", -1.0f);
        next.speedOnline = Boolean(packet, "speed_online", false);
        next.speedAgeMs = Number<float>(packet, "speed_age_ms", nullptr, -1.0f);
        next.rpmLeft = Number<std::uint16_t>(packet, "rpm_left", "rpm_l", next.rpmLeft);
        next.rpmRight = Number<std::uint16_t>(packet, "rpm_right", "rpm_r", next.rpmRight);
        next.motorLeftOk = Boolean(packet, "motor_left_ok", false);
        next.motorRightOk = Boolean(packet, "motor_right_ok", false);
        next.rpmLeftOnline = Boolean(packet, "rpm_left_online", false);
        next.rpmRightOnline = Boolean(packet, "rpm_right_online", false);
        next.rpmLeftAgeMs = Number<float>(packet, "rpm_left_age_ms", nullptr, -1.0f);
        next.rpmRightAgeMs = Number<float>(packet, "rpm_right_age_ms", nullptr, -1.0f);
        next.captureLeft = Number<std::uint32_t>(packet, "cap_left", "capture_left", next.captureLeft);
        next.captureRight = Number<std::uint32_t>(packet, "cap_right", "capture_right", next.captureRight);
        next.tpsRaw = Number<std::uint16_t>(packet, "tps_raw", nullptr, next.tpsRaw);
        next.tpsPercent = Number<float>(packet, "tps_pct", "throttle_pct", next.tpsPercent);
        next.tpsOk = Boolean(packet, "tps_ok", next.tpsOk);
        next.tpsOnline = Boolean(packet, "tps_online", false);
        next.tpsAgeMs = Number<float>(packet, "tps_age_ms", nullptr, -1.0f);
        next.sasRaw = Number<std::uint16_t>(packet, "sas_raw", nullptr, next.sasRaw);
        next.sasCenterRaw = Number<std::uint16_t>(packet, "sas_center_raw", nullptr, next.sasCenterRaw);
        next.sasAbsoluteDeg = Number<float>(packet, "sas_absolute_deg", "sas_abs_deg", next.sasAbsoluteDeg);
        next.sasRelativeDeg = Number<float>(packet, "sas_relative_deg", "sas_sensor_deg", next.sasRelativeDeg);
        next.steeringDeg = Number<float>(packet, "sas_deg", "steering_deg", next.steeringDeg);
        next.sasOk = Boolean(packet, "sas_ok", next.sasOk);
        next.sasOnline = Boolean(packet, "sas_online", false);
        next.sasAgeMs = Number<float>(packet, "sas_age_ms", nullptr, -1.0f);
        next.frontSensorOnline = Boolean(packet, "front_sensor_online", next.frontSensorOnline);
        next.frontDirectOnline = Boolean(packet, "front_direct_online", next.frontDirectOnline);
        next.frontDirectAgeMs = Number<float>(packet, "front_direct_age_ms", nullptr, next.frontDirectAgeMs);
        Text(packet, "front_source", next.frontSource, sizeof(next.frontSource));
        next.yawRateRadS = Number<float>(packet, "yaw_rate_rad_s", "yaw_rate", next.yawRateRadS);
        next.lateralAccelMS2 = Number<float>(packet, "lateral_accel_m_s2", "lat_accel", next.lateralAccelMS2);
        next.longitudinalAccelMS2 = Number<float>(packet, "longitudinal_accel_m_s2", "lon_accel", next.longitudinalAccelMS2);
        next.imuRawAxMS2 = Number<float>(packet, "imu_raw_ax_m_s2", nullptr, next.imuRawAxMS2);
        next.imuRawAyMS2 = Number<float>(packet, "imu_raw_ay_m_s2", nullptr, next.imuRawAyMS2);
        next.imuRawAzMS2 = Number<float>(packet, "imu_raw_az_m_s2", nullptr, next.imuRawAzMS2);
        next.imuOk = Boolean(packet, "imu_ok", false);
        next.imuOnline = Boolean(packet, "imu_online", false);
        next.imuAgeMs = Number<float>(packet, "imu_age_ms", nullptr, -1.0f);
        next.gnssOnline = Boolean(packet, "gnss_online", Boolean(packet, "gps_online", next.gnssOnline));
        next.gnssFixType = Number<std::uint8_t>(packet, "gnss_fix_type", "fix_type", next.gnssFixType);
        next.gnssSatellites = Number<std::uint8_t>(packet, "gnss_satellites", "satellites", next.gnssSatellites);
        next.gnssLatitudeDeg = Number<double>(packet, "gnss_latitude_deg", "latitude", next.gnssLatitudeDeg);
        next.gnssLongitudeDeg = Number<double>(packet, "gnss_longitude_deg", "longitude", next.gnssLongitudeDeg);
        next.gnssAltitudeM = Number<float>(packet, "gnss_altitude_m", "altitude_m", next.gnssAltitudeM);
        next.gnssHeadingDeg = Number<float>(packet, "gnss_heading_deg", "heading_deg", next.gnssHeadingDeg);
        next.gnssHdop = Number<float>(packet, "gnss_hdop", "hdop", next.gnssHdop);
        next.gnssAccuracyM = Number<float>(packet, "gnss_accuracy_m", "accuracy_m", next.gnssAccuracyM);
        Text(packet, "gnss_source", next.gnssSource, sizeof(next.gnssSource));
        next.gnssAgeMs = Number<float>(packet, "gnss_age_ms", nullptr, next.gnssAgeMs);
        if (!std::isfinite(next.gnssLatitudeDeg) || next.gnssLatitudeDeg < -90.0 || next.gnssLatitudeDeg > 90.0 ||
            !std::isfinite(next.gnssLongitudeDeg) || next.gnssLongitudeDeg < -180.0 || next.gnssLongitudeDeg > 180.0) {
            next.gnssOnline = false;
        }
        next.dacLeft = Number<std::uint16_t>(packet, "dac_left", "dac_l", next.dacLeft);
        next.dacRight = Number<std::uint16_t>(packet, "dac_right", "dac_r", next.dacRight);
        next.powerLeftKw = Number<float>(packet, "power_left_kw", nullptr, next.powerLeftKw);
        next.powerRightKw = Number<float>(packet, "power_right_kw", nullptr, next.powerRightKw);
        next.deltaPowerKw = Number<float>(packet, "delta_power_kw", nullptr, next.deltaPowerKw);
        next.vehicleSpeedMS = Number<float>(packet, "vehicle_speed_m_s", nullptr, next.vehicleSpeedMS);
        next.desiredYawRadS = Number<float>(packet, "desired_yaw_rad_s", nullptr, next.desiredYawRadS);
        next.yawErrorRadS = Number<float>(packet, "yaw_error_rad_s", nullptr, next.yawErrorRadS);
        next.tractionScale = Number<float>(packet, "traction_scale", nullptr, next.tractionScale);
        next.edActive = Boolean(packet, "ed_active", next.edActive);
        next.tqvInternalOnline = Boolean(packet, "tqv_internal_online", false);
        next.rearOutputOnline = Boolean(packet, "rear_output_online", false);
        next.rearOutputAgeMs = Number<float>(packet, "rear_output_age_ms", nullptr, -1.0f);
        next.batterySocPercent = Number<float>(packet, "battery_soc_pct", "soc_pct", next.batterySocPercent);
        next.batteryPackVoltageV = Number<float>(packet, "battery_pack_voltage_v", "pack_voltage_v", next.batteryPackVoltageV);
        next.batteryCurrentA = Number<float>(packet, "battery_current_a", "pack_current_a", next.batteryCurrentA);
        next.batteryPowerKw = Number<float>(packet, "battery_power_kw", "pack_power_kw", next.batteryPowerKw);
        next.bmsOnline = Boolean(packet, "bms_online", next.bmsOnline);
        next.bmsOk = Boolean(packet, "bms_ok", next.bmsOk);
        next.bmsFault = Boolean(packet, "bms_fault", next.bmsFault);
        next.bmsChargeMosOn = Boolean(packet, "bms_charge_mos_on", next.bmsChargeMosOn);
        next.bmsDischargeMosOn = Boolean(packet, "bms_discharge_mos_on", next.bmsDischargeMosOn);
        next.bmsChargerPresent = Boolean(packet, "bms_charger_present", next.bmsChargerPresent);
        next.bmsLoadPresent = Boolean(packet, "bms_load_present", next.bmsLoadPresent);
        next.bmsBalancing = Boolean(packet, "bms_balancing", next.bmsBalancing);
        next.bmsAgeMs = Number<float>(packet, "bms_age_ms", nullptr, next.bmsAgeMs);
        next.bmsCellCount = Number<std::uint8_t>(packet, "bms_cell_count", "cell_count", next.bmsCellCount);
        next.bmsTempCount = Number<std::uint8_t>(packet, "bms_temp_count", "temp_count", next.bmsTempCount);
        next.bmsMaxCellNumber = Number<std::uint8_t>(packet, "bms_max_cell_number", nullptr, next.bmsMaxCellNumber);
        next.bmsMinCellNumber = Number<std::uint8_t>(packet, "bms_min_cell_number", nullptr, next.bmsMinCellNumber);
        next.bmsMaxCellVoltageV = Number<float>(packet, "bms_max_cell_voltage_v", nullptr, next.bmsMaxCellVoltageV);
        next.bmsMinCellVoltageV = Number<float>(packet, "bms_min_cell_voltage_v", nullptr, next.bmsMinCellVoltageV);
        next.bmsCellDeltaMv = Number<float>(packet, "bms_cell_delta_mv", nullptr, next.bmsCellDeltaMv);
        next.bmsTempMaxC = Number<float>(packet, "bms_temp_max_c", nullptr, next.bmsTempMaxC);
        next.bmsTempMinC = Number<float>(packet, "bms_temp_min_c", nullptr, next.bmsTempMinC);
        next.bmsRemainingCapacityAh = Number<float>(packet, "bms_remaining_capacity_ah", nullptr, next.bmsRemainingCapacityAh);
        next.bmsCycleCount = Number<std::uint32_t>(packet, "bms_cycle_count", nullptr, next.bmsCycleCount);
        Text(packet, "bms_source", next.bmsSource, sizeof(next.bmsSource));
        Text(packet, "bms_model", next.bmsModel, sizeof(next.bmsModel));
        Text(packet, "bms_protocol", next.bmsProtocol, sizeof(next.bmsProtocol));
        Text(packet, "bms_state", next.bmsState, sizeof(next.bmsState));
        Text(packet, "bms_alarm_hex", next.bmsAlarmHex, sizeof(next.bmsAlarmHex));
        Text(packet, "bms_alarm_summary", next.bmsAlarmSummary, sizeof(next.bmsAlarmSummary));
        if (packet.contains("bms_cell_voltages_v") && packet["bms_cell_voltages_v"].is_array()) {
            next.bmsCellVoltagesV.fill(0.0f);
            next.bmsCellVoltageCount = 0;
            for (const auto& value : packet["bms_cell_voltages_v"]) {
                if (next.bmsCellVoltageCount >= next.bmsCellVoltagesV.size()) break;
                if (!value.is_number()) continue;
                next.bmsCellVoltagesV[next.bmsCellVoltageCount++] = value.get<float>();
            }
        }
        next.tvRequestedPercent = Number<float>(packet, "tv_requested_pct", "tqv_setting_pct", next.tvRequestedPercent);
        next.tvAppliedPercent = Number<float>(packet, "tv_applied_pct", nullptr, next.tvAppliedPercent);
        next.regenRequestedPercent = Number<float>(packet, "regen_requested_pct", nullptr, next.regenRequestedPercent);
        next.regenAppliedPercent = Number<float>(packet, "regen_applied_pct", nullptr, next.regenAppliedPercent);
        next.tvLimitPercent = Number<float>(packet, "tv_limit_pct", nullptr, next.tvLimitPercent);
        next.regenLimitPercent = Number<float>(packet, "regen_limit_pct", nullptr, next.regenLimitPercent);
        next.tvRampPercentPerSecond = Number<float>(packet, "tv_ramp_pct_s", nullptr, next.tvRampPercentPerSecond);
        next.regenRampPercentPerSecond = Number<float>(packet, "regen_ramp_pct_s", nullptr, next.regenRampPercentPerSecond);
        next.driveMode = Number<std::uint8_t>(packet, "drive_mode_id", nullptr, next.driveMode);
        next.driverControlSequence = Number<std::uint8_t>(packet, "driver_control_seq", nullptr, next.driverControlSequence);
        next.rearStatusSequence = Number<std::uint8_t>(packet, "rear_status_seq", nullptr, next.rearStatusSequence);
        next.configSequence = Number<std::uint8_t>(packet, "config_seq", nullptr, next.configSequence);
        next.driverControlFresh = Boolean(packet, "driver_control_fresh", next.driverControlFresh);
        next.tvActive = Boolean(packet, "tv_active", next.tvActive);
        next.regenReady = Boolean(packet, "regen_ready", next.regenReady);
        next.regenActive = Boolean(packet, "regen_active", next.regenActive);
        next.tvLimited = Boolean(packet, "tv_limited", next.tvLimited);
        next.regenLimited = Boolean(packet, "regen_limited", next.regenLimited);
        next.pitAdjustAllowed = Boolean(packet, "pit_adjust_allowed", next.pitAdjustAllowed);
        next.gatewayControlEnabled = Boolean(packet, "gateway_control_enabled", next.gatewayControlEnabled);
        next.canOk = Boolean(packet, "can_ok", next.canOk);
        next.stmOnline = Boolean(packet, "stm_online", false);
        next.usbLinkOnline = Boolean(packet, "usb_link_online", next.usbLinkOnline);
        next.wifiUdpLinkOnline = Boolean(packet, "wifi_udp_link_online", next.wifiUdpLinkOnline);
        next.relayLinkOnline = Boolean(packet, "relay_link_online", next.relayLinkOnline);
        next.relayServerEnabled = Boolean(packet, "relay_server_enabled", next.relayServerEnabled);
        next.wifiConnected = Boolean(packet, "wifi_connected", next.wifiConnected);
        next.internetRelayEnabled = Boolean(packet, "internet_relay_enabled", next.internetRelayEnabled);
        next.relayAgeMs = Number<float>(packet, "relay_age_ms", nullptr, next.relayAgeMs);
        next.wifiRssiDbm = Number<std::int32_t>(packet, "wifi_rssi_dbm", nullptr, next.wifiRssiDbm);
        next.internetRelayTx = Number<std::uint32_t>(packet, "internet_relay_tx", nullptr, next.internetRelayTx);
        next.internetRelayErrors = Number<std::uint32_t>(packet, "internet_relay_errors", nullptr, next.internetRelayErrors);
        next.internetRelayCommands = Number<std::uint32_t>(packet, "internet_relay_commands", nullptr, next.internetRelayCommands);
        next.faultCode = Number<std::uint8_t>(packet, "fault_code", "fault", next.faultCode);
        Text(packet, "telemetry_transport", next.telemetryTransport, sizeof(next.telemetryTransport));
        Text(packet, "relay_vehicle_id", next.relayVehicleId, sizeof(next.relayVehicleId));
        Text(packet, "limit_reason", next.limitReason, sizeof(next.limitReason));
        Text(packet, "command_source", next.commandSource, sizeof(next.commandSource));
        Text(packet, "command_status", next.commandStatus, sizeof(next.commandStatus));
        Text(packet, "command_message", next.commandMessage, sizeof(next.commandMessage));
        next.commandRequestId = Number<std::uint32_t>(
            packet, "command_request_id", nullptr, next.commandRequestId);
        next.recordingActive = Boolean(packet, "recording_active", next.recordingActive);
        next.recordingSessionId = Number<std::uint32_t>(
            packet, "recording_session_id", nullptr, next.recordingSessionId);
        next.recordingSampleCount = Number<std::uint64_t>(
            packet, "recording_sample_count", nullptr, next.recordingSampleCount);
        next.recordingElapsedS = Number<float>(
            packet, "recording_elapsed_s", nullptr, next.recordingElapsedS);
        Text(packet, "database_path", next.databasePath, sizeof(next.databasePath));

        const float reportedSpeed = Number<float>(packet, "speed_kmh", "speed", -1.0f);
        next.speedKmh = reportedSpeed >= 0.0f
            ? reportedSpeed
            : SpeedFromMotorRpm(next.rpmLeft, next.rpmRight);

        const std::uint64_t now = GetTickCount64();
        arrivals_.push_back(now);
        while (!arrivals_.empty() && now - arrivals_.front() > 2000) arrivals_.pop_front();
        next.receiveRateHz = arrivals_.size() > 1
            ? static_cast<float>(arrivals_.size() - 1) * 1000.0f /
                static_cast<float>(arrivals_.back() - arrivals_.front())
            : 0.0f;
        next.receivedPackets++;
        if (next.sequence != 0 && lastSequence_ != 0 && next.sequence > lastSequence_ + 1) {
            next.lostPackets += next.sequence - lastSequence_ - 1;
        }
        if (next.sequence != 0) lastSequence_ = next.sequence;
        next.ageSeconds = 0.0f;

        pendingTelemetry_ = next;
        lastPacketTick_ = now;
    } catch (...) {
        // A malformed datagram must not interrupt the receive loop.
    }
}

void EVDataSource::Update(float /*deltaTime*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_ = pendingTelemetry_;
    const std::uint64_t tick = lastPacketTick_.load();
    telemetry_.ageSeconds = tick == 0
        ? 999.0f
        : static_cast<float>(GetTickCount64() - tick) / 1000.0f;
}

bool EVDataSource::IsConnected() const {
    const std::uint64_t tick = lastPacketTick_.load();
    return tick != 0 && GetTickCount64() - tick < 1500;
}
