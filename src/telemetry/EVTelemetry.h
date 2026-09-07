#pragma once

#include <cstdint>
#include <cstring>
#include <array>

struct EVTelemetry {
    std::uint32_t sequence = 0;
    std::uint32_t timestampMs = 0;

    float speedKmh = 0.0f;
    std::uint16_t rpmLeft = 0;
    std::uint16_t rpmRight = 0;
    std::uint32_t captureLeft = 0;
    std::uint32_t captureRight = 0;

    std::uint16_t tpsRaw = 0;
    float tpsPercent = 0.0f;
    bool tpsOk = false;
    std::uint16_t sasRaw = 0;
    std::uint16_t sasCenterRaw = 8192;
    float sasAbsoluteDeg = 0.0f;
    float sasRelativeDeg = 0.0f;
    float steeringDeg = 0.0f;
    bool sasOk = false;
    bool frontSensorOnline = false;
    bool frontDirectOnline = false;
    float frontDirectAgeMs = 0.0f;
    char frontSource[32] = "CAN / ESP";
    float yawRateRadS = 0.0f;
    float lateralAccelMS2 = 0.0f;

    // Reserved for a real GNSS receiver. The dashboard never generates
    // synthetic coordinates when this input is absent.
    bool gnssOnline = false;
    std::uint8_t gnssFixType = 0;
    std::uint8_t gnssSatellites = 0;
    double gnssLatitudeDeg = 0.0;
    double gnssLongitudeDeg = 0.0;
    float gnssAltitudeM = 0.0f;
    float gnssHeadingDeg = 0.0f;
    float gnssHdop = 0.0f;
    float gnssAgeMs = 0.0f;

    std::uint16_t dacLeft = 0;
    std::uint16_t dacRight = 0;
    float powerLeftKw = 0.0f;
    float powerRightKw = 0.0f;
    float deltaPowerKw = 0.0f;

    float batterySocPercent = 0.0f;
    float batteryPackVoltageV = 0.0f;
    float batteryCurrentA = 0.0f;
    float batteryPowerKw = 0.0f;
    bool bmsOnline = false;
    bool bmsOk = false;
    bool bmsFault = false;
    bool bmsChargeMosOn = false;
    bool bmsDischargeMosOn = false;
    bool bmsChargerPresent = false;
    bool bmsLoadPresent = false;
    bool bmsBalancing = false;
    float bmsAgeMs = 0.0f;
    std::uint8_t bmsCellCount = 0;
    std::uint8_t bmsTempCount = 0;
    std::uint8_t bmsMaxCellNumber = 0;
    std::uint8_t bmsMinCellNumber = 0;
    float bmsMaxCellVoltageV = 0.0f;
    float bmsMinCellVoltageV = 0.0f;
    float bmsCellDeltaMv = 0.0f;
    float bmsTempMaxC = 0.0f;
    float bmsTempMinC = 0.0f;
    float bmsRemainingCapacityAh = 0.0f;
    std::uint32_t bmsCycleCount = 0;
    std::array<float, 48> bmsCellVoltagesV = {};
    std::uint8_t bmsCellVoltageCount = 0;
    char bmsSource[32] = "NONE";
    char bmsModel[32] = "";
    char bmsProtocol[32] = "";
    char bmsState[16] = "UNKNOWN";
    char bmsAlarmHex[32] = "";
    char bmsAlarmSummary[96] = "NONE";

    // 0x110 is the driver's physical wheel-dial request. 0x310 is what the
    // rear STM actually accepted after freshness, limits, and ramping.
    float tvRequestedPercent = 0.0f;
    float tvAppliedPercent = 0.0f;
    float regenRequestedPercent = 0.0f;
    float regenAppliedPercent = 0.0f;
    float tvLimitPercent = 100.0f;
    float regenLimitPercent = 0.0f;
    float tvRampPercentPerSecond = 200.0f;
    float regenRampPercentPerSecond = 0.0f;
    std::uint8_t driveMode = 1;
    std::uint8_t driverControlSequence = 0;
    std::uint8_t rearStatusSequence = 0;
    std::uint8_t configSequence = 0;
    bool driverControlFresh = false;
    bool tvActive = false;
    bool regenReady = false;
    bool regenActive = false;
    bool tvLimited = false;
    bool regenLimited = false;
    bool pitAdjustAllowed = false;
    bool gatewayControlEnabled = false;
    bool canOk = false;
    bool stmOnline = false;
    bool usbLinkOnline = false;
    bool wifiUdpLinkOnline = false;
    bool relayLinkOnline = false;
    bool relayServerEnabled = false;
    bool wifiConnected = false;
    bool internetRelayEnabled = false;
    float relayAgeMs = -1.0f;
    std::int32_t wifiRssiDbm = -127;
    std::uint32_t internetRelayTx = 0;
    std::uint32_t internetRelayErrors = 0;
    std::uint32_t internetRelayCommands = 0;
    std::uint8_t faultCode = 0;
    char telemetryTransport[24] = "NONE";
    char relayVehicleId[32] = "";
    char limitReason[64] = "NONE";
    char commandSource[16] = "WHEEL";
    char commandStatus[16] = "IDLE";
    char commandMessage[96] = "";
    std::uint32_t commandRequestId = 0;

    std::uint64_t receivedPackets = 0;
    std::uint64_t lostPackets = 0;
    float receiveRateHz = 0.0f;
    float ageSeconds = 0.0f;
};

inline const char* EVDriveModeName(std::uint8_t mode) {
    switch (mode) {
        case 0: return "예선";
        case 1: return "주행";
        case 2: return "충전";
        case 3: return "어택";
        default: return "미확인";
    }
}

inline const char* EVFaultName(std::uint8_t code) {
    switch (code) {
        case 0: return "정상";
        case 1: return "CAN 시간 초과";
        case 2: return "TPS 범위 오류";
        case 3: return "IMU 데이터 오류";
        case 4: return "RPM 시간 초과";
        default: return "미확인 오류";
    }
}
