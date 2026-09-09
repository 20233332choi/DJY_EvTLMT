#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <esp32-hal-rgb-led.h>
#include <driver/twai.h>

#include "config.h"
#include "djy_can_protocol.h"
#include "djy_uart_protocol.h"

#ifndef EV_REAR_UART_MODE
#define EV_REAR_UART_MODE 0
#endif

#ifndef EV_REAR_UART_RX_GPIO
#define EV_REAR_UART_RX_GPIO 18
#endif

#ifndef EV_REAR_UART_TX_GPIO
#define EV_REAR_UART_TX_GPIO 17
#endif

#ifndef EV_LOCAL_UDP_ENABLED
#define EV_LOCAL_UDP_ENABLED 1
#endif

#ifndef EV_RELAY_ENABLED
#define EV_RELAY_ENABLED 0
#endif

#ifndef EV_RELAY_URL
#define EV_RELAY_URL ""
#endif

#ifndef EV_RELAY_TOKEN
#define EV_RELAY_TOKEN ""
#endif

#ifndef EV_VEHICLE_ID
#define EV_VEHICLE_ID "EV"
#endif

#ifndef EV_RELAY_ALLOW_INSECURE_TLS
#define EV_RELAY_ALLOW_INSECURE_TLS 0
#endif

#ifndef EV_RELAY_ACCEPT_COMMANDS
#define EV_RELAY_ACCEPT_COMMANDS 0
#endif

#ifndef EV_ALLOW_INSECURE_RELAY_COMMANDS
#define EV_ALLOW_INSECURE_RELAY_COMMANDS 0
#endif

#ifndef EV_ALLOW_PLAINTEXT_RELAY
#define EV_ALLOW_PLAINTEXT_RELAY 0
#endif

#ifndef EV_STATUS_LED_ENABLED
#define EV_STATUS_LED_ENABLED 1
#endif

#ifndef EV_STATUS_LED_GPIO
// Some ESP32-S3 DevKitC-1 boards cannot drive the GPIO48 NeoPixel data line
// high enough. Jumper GPIO38 to GPIO48, then drive the RGB LED through GPIO38.
#define EV_STATUS_LED_GPIO 38
#endif

#if EV_RELAY_ENABLED && EV_RELAY_ACCEPT_COMMANDS && EV_RELAY_ALLOW_INSECURE_TLS && !EV_ALLOW_INSECURE_RELAY_COMMANDS
#error "Internet vehicle commands require verified TLS; do not combine command RX with insecure TLS"
#endif

#if EV_RELAY_ENABLED && EV_RELAY_ACCEPT_COMMANDS && EV_ALLOW_PLAINTEXT_RELAY && !EV_ALLOW_INSECURE_RELAY_COMMANDS
#error "Internet vehicle commands require verified TLS; do not combine command RX with plaintext HTTP"
#endif

namespace {

WiFiUDP telemetryUdp;
WiFiUDP commandUdp;
IPAddress pitHost;
bool canReady = false;
HardwareSerial rearUart(1);
bool rearUartReady = false;
constexpr uint16_t kSasCounts = 16384u;
constexpr uint16_t kSasCenterRaw = 8192u;
constexpr float kSasToSteeringRatio = -0.2f;

struct VehicleState {
    uint32_t sequence = 0;
    uint16_t sasRaw = 0, tpsRaw = 0;
    uint16_t rpmLeft = 0, rpmRight = 0;
    uint16_t dacLeft = 0, dacRight = 0;
    uint8_t tvRequested = 0, regenRequested = 0;
    uint8_t tvApplied = 0, regenApplied = 0;
    uint8_t driveMode = 1, controlFlags = 0, rearFlags = 0, fault = 0;
    uint8_t driverSequence = 0, rearSequence = 0, configSequence = 0;
    uint8_t tvLimit = 100, regenLimit = 0;
    uint16_t tvRamp = 200, regenRamp = 0;
    bool configAck = false;
    bool imuValid = false;
    uint32_t rearUartRx = 0, rearUartErrors = 0;
    int16_t tpsReportedPct = -1;
    uint32_t sasMs = 0, tpsMs = 0, driverMs = 0, rearMs = 0;
} state;

DjyUartLiveTv liveCommand = {};
bool liveCommandSeen = false;
uint32_t liveCommandMs = 0;

bool jsonStringEquals(const String& input, const char* key, const char* expected) {
    const String marker = String('"') + key + "\":";
    int start = input.indexOf(marker);
    if (start < 0) return false;
    start += marker.length();
    while (start < input.length() && input[start] == ' ') ++start;
    if (start >= input.length() || input[start] != '"') return false;
    ++start;
    const int end = input.indexOf('"', start);
    return end >= start && input.substring(start, end) == expected;
}

#if EV_RELAY_ENABLED
portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;
char relayTelemetry[1536] = {};
uint32_t relayTelemetryVersion = 0;
char relayCommand[512] = {};
bool relayCommandPending = false;
uint32_t relayTxCount = 0;
uint32_t relayErrorCount = 0;
uint32_t relayCommandCount = 0;
int relayLastHttpStatus = 0;
int relayDnsStatus = 0;
int relayTcp443Status = 0;
int relayTlsError = 0;

void stageRelayTelemetry(const char* json) {
    portENTER_CRITICAL(&relayMux);
    strlcpy(relayTelemetry, json, sizeof(relayTelemetry));
    ++relayTelemetryVersion;
    portEXIT_CRITICAL(&relayMux);
}

void stageRelayCommand(const String& response) {
#if EV_RELAY_ACCEPT_COMMANDS
    if (!jsonStringEquals(response, "type", "live_tv") &&
        !jsonStringEquals(response, "type", "pit_config") &&
        !jsonStringEquals(response, "type", "relay_ping")) return;
    portENTER_CRITICAL(&relayMux);
    strlcpy(relayCommand, response.c_str(), sizeof(relayCommand));
    relayCommandPending = true;
    ++relayCommandCount;
    portEXIT_CRITICAL(&relayMux);
#else
    (void)response;
#endif
}

void relayTask(void*) {
    const String relayUrl(EV_RELAY_URL);
    const String relayToken(EV_RELAY_TOKEN);
    const bool relayUsesTls = relayUrl.startsWith("https://");
    const bool relayUsesPlaintext = relayUrl.startsWith("http://");
    if ((!relayUsesTls && !relayUsesPlaintext) || relayToken.length() < 16u) {
        Serial.println("# Internet relay disabled: configure URL and a token of at least 16 characters");
        vTaskDelete(nullptr);
        return;
    }
    if (relayUsesPlaintext && !EV_ALLOW_PLAINTEXT_RELAY) {
        Serial.println("# HTTP relay disabled: set EV_ALLOW_PLAINTEXT_RELAY for bench use");
        vTaskDelete(nullptr);
        return;
    }

    const int authorityStart = relayUrl.indexOf("//") + 2;
    const int pathStart = relayUrl.indexOf('/', authorityStart);
    String relayAuthority = pathStart >= 0 ? relayUrl.substring(authorityStart, pathStart)
                                           : relayUrl.substring(authorityStart);
    uint16_t relayPort = relayUsesTls ? 443 : 80;
    const int portMarker = relayAuthority.lastIndexOf(':');
    if (portMarker > 0) {
        relayPort = static_cast<uint16_t>(relayAuthority.substring(portMarker + 1).toInt());
        relayAuthority = relayAuthority.substring(0, portMarker);
    }

    uint32_t sentVersion = 0;
    uint32_t retryMs = 1000;
    bool networkPathChecked = false;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            networkPathChecked = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!networkPathChecked) {
            IPAddress relayAddress;
            const int dnsOk = WiFi.hostByName(relayAuthority.c_str(), relayAddress) == 1 ? 1 : -1;
            int tcpOk = 0;
            if (dnsOk == 1) {
                WiFiClient tcpProbe;
                tcpOk = tcpProbe.connect(relayAddress, relayPort, 5000) == 1 ? 1 : -1;
                tcpProbe.stop();
            }
            portENTER_CRITICAL(&relayMux);
            relayDnsStatus = dnsOk;
            relayTcp443Status = tcpOk;
            portEXIT_CRITICAL(&relayMux);
            networkPathChecked = true;
        }

        char payload[1536];
        uint32_t pendingVersion = 0;
        portENTER_CRITICAL(&relayMux);
        pendingVersion = relayTelemetryVersion;
        strlcpy(payload, relayTelemetry, sizeof(payload));
        portEXIT_CRITICAL(&relayMux);
        if (pendingVersion == 0u || pendingVersion == sentVersion || payload[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Baja relay compatibility: one DNS/TCP/TLS/HTTP exchange per socket.
        // ngrok can acknowledge keep-alive while delaying a reused TLS socket.
        WiFiClient plainClient;
        WiFiClientSecure secureClient;
        WiFiClient* relayClient = relayUsesTls ? static_cast<WiFiClient*>(&secureClient) : &plainClient;
        if (relayUsesTls) {
#if defined(EV_RELAY_CA_CERT)
            secureClient.setCACert(EV_RELAY_CA_CERT);
#elif EV_RELAY_ALLOW_INSECURE_TLS
            secureClient.setInsecure();
#else
            Serial.println("# HTTPS relay disabled: configure EV_RELAY_CA_CERT");
            vTaskDelete(nullptr);
            return;
#endif
            // Keep failed cellular/ngrok handshakes bounded.  The relay task
            // runs at idle priority below so a slow TLS peer cannot starve
            // the ESP-IDF idle task and trip the core watchdog.
            secureClient.setHandshakeTimeout(7);
        }

        HTTPClient http;
        http.useHTTP10(true);
        http.setReuse(false);
        http.setConnectTimeout(7000);
        http.setTimeout(7000);
        bool success = false;
        if (http.begin(*relayClient, relayUrl)) {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + relayToken);
            http.addHeader("X-Vehicle-ID", EV_VEHICLE_ID);
            http.addHeader("ngrok-skip-browser-warning", "1");
            http.addHeader("Connection", "close");
            const int status = http.POST(reinterpret_cast<uint8_t*>(payload), strlen(payload));
            char tlsErrorText[96] = {};
            const int tlsError = relayUsesTls && status < 0
                                     ? secureClient.lastError(tlsErrorText, sizeof(tlsErrorText))
                                     : 0;
            portENTER_CRITICAL(&relayMux);
            relayLastHttpStatus = status;
            relayTlsError = tlsError;
            portEXIT_CRITICAL(&relayMux);
            if (status >= 200 && status < 300) {
                const String response = http.getString();
                stageRelayCommand(response);
                sentVersion = pendingVersion;
                portENTER_CRITICAL(&relayMux);
                ++relayTxCount;
                portEXIT_CRITICAL(&relayMux);
                retryMs = 1000;
                success = true;
            }
            http.end();
        } else {
            portENTER_CRITICAL(&relayMux);
            relayLastHttpStatus = -1000;
            portEXIT_CRITICAL(&relayMux);
        }
        relayClient->stop();
        if (!success) {
            portENTER_CRITICAL(&relayMux);
            ++relayErrorCount;
            portEXIT_CRITICAL(&relayMux);
            vTaskDelay(pdMS_TO_TICKS(retryMs));
            retryMs = min(retryMs * 2u, 5000u);
        } else {
            // A fresh TCP connection per exchange is ngrok-compatible, but
            // cap it near 4 Hz to avoid exhausting free-tunnel connection rate.
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}
#endif

void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
#if EV_STATUS_LED_ENABLED
    neopixelWrite(EV_STATUS_LED_GPIO, red, green, blue);
#else
    (void)red;
    (void)green;
    (void)blue;
#endif
}

void runStatusLedSelfTest() {
#if EV_STATUS_LED_ENABLED
    setStatusLed(255, 0, 0);
    delay(350);
    setStatusLed(0, 255, 0);
    delay(350);
    setStatusLed(0, 0, 255);
    delay(350);
    setStatusLed(0, 0, 0);
#endif
}

void updateStatusLed() {
#if EV_STATUS_LED_ENABLED
    const uint32_t now = millis();
    static uint32_t previous = 0;
    if (now - previous < 100u) return;
    previous = now;

    const bool blink = (now / 500u) % 2u == 0u;
    if (WiFi.status() != WL_CONNECTED) {
        setStatusLed(0, 0, blink ? 64 : 0);  // Blue: joining hotspot.
        return;
    }

#if EV_RELAY_ENABLED
    uint32_t relayTx = 0;
    portENTER_CRITICAL(&relayMux);
    relayTx = relayTxCount;
    portEXIT_CRITICAL(&relayMux);
    if (relayTx > 0u) {
        setStatusLed(0, 64, 0);  // Green: live HTTPS telemetry upload.
    } else {
        setStatusLed(blink ? 64 : 0, blink ? 32 : 0, 0);  // Yellow: relay retrying.
    }
#else
    setStatusLed(0, 48, 48);  // Cyan: local Wi-Fi/UDP mode only.
#endif
#endif
}

float throttlePercent() {
    if (state.tpsReportedPct >= 0) return static_cast<float>(state.tpsReportedPct);
    constexpr float kMin = 868.0f, kMax = 3102.0f;
    return constrain((static_cast<float>(state.tpsRaw) - kMin) * 100.0f / (kMax - kMin), 0.0f, 100.0f);
}

int16_t sasWrappedDelta() {
    int32_t delta = static_cast<int32_t>(state.sasRaw) - static_cast<int32_t>(kSasCenterRaw);
    if (delta > 8191) delta -= kSasCounts;
    if (delta < -8192) delta += kSasCounts;
    return static_cast<int16_t>(delta);
}

float sasAbsoluteDeg() {
    return static_cast<float>(state.sasRaw) * 360.0f / static_cast<float>(kSasCounts);
}

float sasRelativeDeg() {
    return static_cast<float>(sasWrappedDelta()) * 360.0f / static_cast<float>(kSasCounts);
}

float speedKmh() {
    constexpr float kGearRatio = 3.8f, kTireRadiusM = 0.2286f;
    const float motorRpm = 0.5f * (state.rpmLeft + state.rpmRight);
    return motorRpm / kGearRatio * (2.0f * PI * kTireRadiusM) / 60.0f * 3.6f;
}

bool pitAllowed() {
    return canReady && state.rearMs != 0u && digitalRead(EV_PIT_ENABLE_GPIO) == LOW &&
           millis() - state.rearMs < 300u &&
           throttlePercent() <= 2.0f && state.rpmLeft <= 30u && state.rpmRight <= 30u;
}

void sendEspStatus() {
    static uint32_t previous = 0;
    static uint8_t sequence = 0;
    const uint32_t now = millis();
    if (!canReady || now - previous < 100u) return;
    previous = now;

    DjyEspStatus status = {};
    if (digitalRead(EV_PIT_ENABLE_GPIO) == LOW) status.flags |= DJY_ESP_STATUS_PIT_ENABLE;
    if (WiFi.status() == WL_CONNECTED) status.flags |= DJY_ESP_STATUS_WIFI_CONNECTED;
    status.flags |= DJY_ESP_STATUS_CAN_RUNNING;
    status.sequence = sequence++;
    status.uptime_100ms = static_cast<uint16_t>((now / 100u) & 0xffffu);

    twai_message_t frame = {};
    frame.identifier = DJY_CAN_ID_ESP_STATUS;
    frame.data_length_code = DJY_CAN_DLC_ESP_STATUS;
    djy_pack_esp_status(frame.data, status);
    (void)twai_transmit(&frame, pdMS_TO_TICKS(5));
}

int jsonInteger(const String& input, const char* key, int fallback) {
    String marker = String('"') + key + "\":";
    int start = input.indexOf(marker);
    if (start < 0) return fallback;
    start += marker.length();
    while (start < input.length() && input[start] == ' ') ++start;
    return input.substring(start).toInt();
}

bool jsonBoolean(const String& input, const char* key, bool fallback) {
    String marker = String('"') + key + "\":";
    int start = input.indexOf(marker);
    if (start < 0) return fallback;
    start += marker.length();
    while (start < input.length() && input[start] == ' ') ++start;
    if (input.startsWith("true", start)) return true;
    if (input.startsWith("false", start)) return false;
    return fallback;
}

void receiveCan() {
    twai_message_t frame = {};
    while (twai_receive(&frame, 0) == ESP_OK) {
        if (frame.extd || frame.rtr) continue;
        const uint32_t now = millis();
        if (frame.identifier == DJY_CAN_ID_SENSOR_DATA && frame.data_length_code == 4) {
            state.sasRaw = djy_u16(&frame.data[0]) & 0x3fffu;
            state.tpsRaw = djy_u16(&frame.data[2]) & 0x0fffu;
            state.tpsReportedPct = -1;
            state.sasMs = now;
            state.tpsMs = now;
        } else if (frame.identifier == DJY_CAN_ID_DRIVER_CONTROL && frame.data_length_code == 8 &&
                   frame.data[5] == DJY_PROTOCOL_VERSION && frame.data[7] == djy_crc8(frame.data, 7)) {
            state.tvRequested = frame.data[0]; state.regenRequested = frame.data[1];
            state.driveMode = frame.data[2]; state.controlFlags = frame.data[3];
            state.driverSequence = frame.data[4]; state.driverMs = now;
        } else if (frame.identifier == DJY_CAN_ID_REAR_STATUS && frame.data_length_code == 8 &&
                   frame.data[6] == DJY_PROTOCOL_VERSION && frame.data[7] == djy_crc8(frame.data, 7)) {
            state.tvApplied = frame.data[0]; state.regenApplied = frame.data[1];
            state.driveMode = frame.data[2]; state.rearFlags = frame.data[3];
            state.fault = frame.data[4]; state.rearSequence = frame.data[5]; state.rearMs = now;
        } else if (frame.identifier == DJY_CAN_ID_REAR_DRIVETRAIN && frame.data_length_code == 8) {
            state.rpmLeft = djy_u16(&frame.data[0]); state.rpmRight = djy_u16(&frame.data[2]);
            state.dacLeft = djy_u16(&frame.data[4]); state.dacRight = djy_u16(&frame.data[6]);
        } else if (frame.identifier == DJY_CAN_ID_PIT_CONFIG_ACK && frame.data_length_code == 8 &&
                   frame.data[6] == DJY_PROTOCOL_VERSION && frame.data[7] == djy_crc8(frame.data, 7)) {
            state.tvLimit = frame.data[0]; state.regenLimit = frame.data[1];
            state.tvRamp = static_cast<uint16_t>(frame.data[2]) * 10u;
            state.regenRamp = static_cast<uint16_t>(frame.data[3]) * 5u;
            state.configSequence = frame.data[5]; state.configAck = true;
        }
    }
}

void parseRearUartLine(const char* line) {
    unsigned rpmLeft = 0, rpmRight = 0, tpsRaw = 0, idleRaw = 0;
    unsigned espFresh = 0, espSequence = 0;
    unsigned long captureLeft = 0, captureRight = 0;
    unsigned long glitchLeft = 0, glitchRight = 0, espRxCount = 0;
    int tpsPct = 0;
    const int fields = sscanf(
        line,
        "L=%u R=%u cap=%lu/%lu glt=%lu/%lu tps=%u pct=%d idle=%u esp=%u/%u rx=%lu",
        &rpmLeft, &rpmRight, &captureLeft, &captureRight, &glitchLeft, &glitchRight,
        &tpsRaw, &tpsPct, &idleRaw, &espFresh, &espSequence, &espRxCount);
    // Older installed Rear firmware ends after idle=<raw> (9 fields).
    // Newer builds append esp=<fresh>/<seq> rx=<count> (12 fields).
    if (fields < 9) return;

    (void)captureLeft;
    (void)captureRight;
    (void)glitchLeft;
    (void)glitchRight;
    (void)idleRaw;
    (void)espFresh;
    (void)espSequence;
    (void)espRxCount;

    state.rpmLeft = static_cast<uint16_t>(constrain(rpmLeft, 0u, 65535u));
    state.rpmRight = static_cast<uint16_t>(constrain(rpmRight, 0u, 65535u));
    state.tpsRaw = static_cast<uint16_t>(constrain(tpsRaw, 0u, 65535u));
    state.tpsReportedPct = static_cast<int16_t>(constrain(tpsPct, 0, 100));
    state.rearSequence++;
    const uint32_t now = millis();
    state.tpsMs = now;
    state.rearMs = now;

    unsigned controlFresh = 0, controlSequence = 0, requested = 0, limit = 100, applied = 0;
    unsigned tvActive = 0, edActive = 0, fault = 0, dacLeft = 0, dacRight = 0;
    unsigned sasRaw = 0, imuValid = 0;
    unsigned long uartRx = 0, uartErrors = 0;
    const char* control = strstr(line, " ctl=");
    if (control != nullptr && sscanf(
            control,
            " ctl=%u/%u req=%u lim=%u app=%u tv=%u ed=%u fault=%u dac=%u/%u sas=%u imu=%u urx=%lu uerr=%lu",
            &controlFresh, &controlSequence, &requested, &limit, &applied,
            &tvActive, &edActive, &fault, &dacLeft, &dacRight, &sasRaw,
            &imuValid, &uartRx, &uartErrors) == 14) {
        state.tvRequested = static_cast<uint8_t>(constrain(requested, 0u, 100u));
        state.tvLimit = static_cast<uint8_t>(constrain(limit, 0u, 100u));
        state.tvApplied = static_cast<uint8_t>(constrain(applied, 0u, 100u));
        state.configSequence = static_cast<uint8_t>(controlSequence);
        state.configAck = liveCommandSeen && state.configSequence == liveCommand.sequence;
        state.driverMs = controlFresh ? now : 0u;
        state.rearFlags = 0u;
        if (controlFresh) state.rearFlags |= DJY_REAR_STATUS_CONTROL_FRESH;
        if (tvActive) state.rearFlags |= DJY_REAR_STATUS_TV_ACTIVE;
        if (edActive) state.rearFlags |= DJY_REAR_STATUS_ED_ACTIVE;
        if (fault) state.rearFlags |= DJY_REAR_STATUS_FAULT;
        state.fault = static_cast<uint8_t>(fault);
        state.dacLeft = static_cast<uint16_t>(constrain(dacLeft, 0u, 4095u));
        state.dacRight = static_cast<uint16_t>(constrain(dacRight, 0u, 4095u));
        state.sasRaw = static_cast<uint16_t>(sasRaw & 0x3fffu);
        state.sasMs = now;
        state.imuValid = imuValid != 0u;
        state.rearUartRx = static_cast<uint32_t>(uartRx);
        state.rearUartErrors = static_cast<uint32_t>(uartErrors);
    }
}

void receiveRearUart() {
    static char line[160];
    static size_t length = 0;
    while (rearUart.available()) {
        const char value = static_cast<char>(rearUart.read());
        if (value == '\n') {
            line[length] = '\0';
            parseRearUartLine(line);
            length = 0;
        } else if (value != '\r') {
            if (length + 1u < sizeof(line)) {
                line[length++] = value;
            } else {
                length = 0;
            }
        }
    }
}

void sendPitConfig(const String& input) {
    if (!pitAllowed()) return;
    twai_message_t frame = {};
    frame.identifier = DJY_CAN_ID_PIT_CONFIG;
    frame.data_length_code = 8;
    frame.data[0] = constrain(jsonInteger(input, "tv_limit_pct", state.tvLimit), 0, 100);
    frame.data[1] = constrain(jsonInteger(input, "regen_limit_pct", state.regenLimit), 0, 100);
    frame.data[2] = constrain(jsonInteger(input, "tv_ramp_pct_s", 200) / 10, 1, 25);
    frame.data[3] = constrain(jsonInteger(input, "regen_ramp_pct_s", 50) / 5, 1, 20);
    frame.data[4] = (jsonBoolean(input, "tv_enable", true) ? DJY_PIT_FLAG_TV_PERMITTED : 0u) |
                    (jsonBoolean(input, "regen_enable", false) ? DJY_PIT_FLAG_REGEN_PERMITTED : 0u);
    frame.data[5] = static_cast<uint8_t>(jsonInteger(input, "request_id", state.configSequence + 1));
    frame.data[6] = DJY_PROTOCOL_VERSION;
    frame.data[7] = djy_crc8(frame.data, 7);
    state.configAck = false;
    (void)twai_transmit(&frame, pdMS_TO_TICKS(5));
}

void acceptLiveTvCommand(const String& input) {
    const int requested = jsonInteger(input, "strength_pct", -1);
    const int limit = jsonInteger(input, "limit_pct", -1);
    if (requested < 0 || requested > 100 || limit < 0 || limit > 100 ||
        (requested % 5) != 0 || (limit % 5) != 0) {
        return;
    }
    const uint8_t sequence = static_cast<uint8_t>(jsonInteger(input, "request_id", 0));
    const uint8_t strength = static_cast<uint8_t>(requested);
    const uint8_t maximum = static_cast<uint8_t>(limit);
    const uint8_t flags = jsonBoolean(input, "tv_enable", true) ? DJY_UART_TV_FLAG_ENABLE : 0u;
    const bool commandChanged = !liveCommandSeen || liveCommand.sequence != sequence ||
                                liveCommand.strength_percent != strength ||
                                liveCommand.limit_percent != maximum || liveCommand.flags != flags;
    liveCommand.sequence = sequence;
    liveCommand.strength_percent = strength;
    liveCommand.limit_percent = maximum;
    liveCommand.flags = flags;
    liveCommandSeen = true;
    liveCommandMs = millis();
    state.tvRequested = liveCommand.strength_percent;
    state.tvLimit = liveCommand.limit_percent;
    if (commandChanged) state.configAck = false;
}

void forwardLiveTvCommand() {
    static uint32_t previous = 0u;
    const uint32_t now = millis();
    if (!rearUartReady || !liveCommandSeen || now - liveCommandMs > 500u ||
        now - previous < 100u) {
        return;
    }
    previous = now;
    uint8_t frame[DJY_UART_LIVE_TV_SIZE];
    djy_uart_pack_live_tv(frame, &liveCommand);
    rearUart.write(frame, sizeof(frame));
}

void handleCommand(const String& input) {
    if (jsonStringEquals(input, "type", "live_tv")) {
        acceptLiveTvCommand(input);
    } else if (jsonStringEquals(input, "type", "pit_config")) {
        sendPitConfig(input);
    }
}

void receiveCommands() {
    int length = commandUdp.parsePacket();
    if (length > 0 && length < 512) {
        const bool trustedPit = commandUdp.remoteIP() == pitHost;
        String input;
        input.reserve(length);
        while (commandUdp.available()) input += static_cast<char>(commandUdp.read());
        if (trustedPit) handleCommand(input);
    }
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        handleCommand(input);
    }
#if EV_RELAY_ENABLED && EV_RELAY_ACCEPT_COMMANDS
    char pending[sizeof(relayCommand)] = {};
    bool available = false;
    portENTER_CRITICAL(&relayMux);
    if (relayCommandPending) {
        strlcpy(pending, relayCommand, sizeof(pending));
        relayCommandPending = false;
        available = true;
    }
    portEXIT_CRITICAL(&relayMux);
    if (available) handleCommand(String(pending));
#endif
}

void publishTelemetry() {
    static uint32_t previous = 0;
    if (millis() - previous < 20u) return;
    previous = millis();
    ++state.sequence;
    const bool driverFresh = state.driverMs != 0u && millis() - state.driverMs < 200u;
    const bool rearFresh = state.rearMs != 0u && millis() - state.rearMs < 300u;
    const bool sasFresh = state.sasMs != 0u && millis() - state.sasMs < 200u;
    const bool tpsFresh = state.tpsMs != 0u && millis() - state.tpsMs < 300u;
    const bool tpsOk = tpsFresh && state.tpsRaw >= 818u && state.tpsRaw <= 3152u;
    const float sasRelative = sasFresh ? sasRelativeDeg() : 0.0f;
    const bool tvLimited = state.tvApplied + 1u < state.tvRequested;
    const bool regenReady = (state.rearFlags & DJY_REAR_STATUS_REGEN_READY) != 0;
    const char* reason = EV_REAR_UART_MODE && !driverFresh ? "PIT_CONTROL_TIMEOUT" :
                         !driverFresh ? "DRIVER_CONTROL_TIMEOUT" :
                         !rearFresh ? "REAR_STATUS_TIMEOUT" :
                         (!regenReady && state.regenRequested > 0) ? "REGEN_INTERFACE_UNVALIDATED" :
                         tvLimited ? "PIT_LIMIT_OR_RAMP" : "NONE";

    uint32_t relayTx = 0, relayErrors = 0, relayCommands = 0;
    int relayHttpStatus = 0, relayDns = 0, relayTcp443 = 0, relayTls = 0;
#if EV_RELAY_ENABLED
    portENTER_CRITICAL(&relayMux);
    relayTx = relayTxCount;
    relayErrors = relayErrorCount;
    relayCommands = relayCommandCount;
    relayHttpStatus = relayLastHttpStatus;
    relayDns = relayDnsStatus;
    relayTcp443 = relayTcp443Status;
    relayTls = relayTlsError;
    portEXIT_CRITICAL(&relayMux);
#endif
    char json[1536];
    snprintf(json, sizeof(json),
        "{\"seq\":%lu,\"timestamp_ms\":%lu,\"rpm_left\":%u,\"rpm_right\":%u,"
        "\"speed_kmh\":%.2f,\"tps_raw\":%u,\"tps_pct\":%.2f,\"dac_left\":%u,\"dac_right\":%u,"
        "\"tps_ok\":%s,\"sas_raw\":%u,\"sas_center_raw\":%u,\"sas_absolute_deg\":%.2f,"
        "\"sas_relative_deg\":%.2f,\"sas_deg\":%.2f,\"sas_ok\":%s,\"front_sensor_online\":%s,\"front_source\":\"%s\","
        "\"tv_requested_pct\":%u,\"tv_applied_pct\":%u,\"regen_requested_pct\":%u,\"regen_applied_pct\":%u,"
        "\"tv_limit_pct\":%u,\"regen_limit_pct\":%u,\"tv_ramp_pct_s\":%u,\"regen_ramp_pct_s\":%u,"
        "\"drive_mode\":%u,\"driver_control_seq\":%u,\"rear_status_seq\":%u,\"config_seq\":%u,\"config_ack\":%s,"
        "\"driver_control_fresh\":%s,\"tv_active\":%s,\"regen_ready\":%s,\"regen_active\":%s,"
        "\"tv_limited\":%s,\"regen_limited\":%s,\"pit_adjust_allowed\":%s,"
        "\"can_ok\":%s,\"uart_ok\":%s,\"stm_online\":%s,\"imu_ok\":%s,\"fault_code\":%u,"
        "\"rear_uart_rx\":%lu,\"rear_uart_errors\":%lu,\"limit_reason\":\"%s\","
        "\"command_source\":\"%s\",\"live_control_allowed\":%s,"
        "\"wifi_connected\":%s,\"wifi_rssi_dbm\":%ld,\"internet_relay_enabled\":%s,"
        "\"internet_relay_tx\":%lu,\"internet_relay_errors\":%lu,\"internet_relay_commands\":%lu,"
        "\"internet_relay_last_http_status\":%d,\"internet_relay_dns_ok\":%d,"
        "\"internet_relay_tcp443_ok\":%d,\"internet_relay_tls_error\":%d}",
        static_cast<unsigned long>(state.sequence), static_cast<unsigned long>(millis()),
        state.rpmLeft, state.rpmRight, speedKmh(), state.tpsRaw, throttlePercent(), state.dacLeft, state.dacRight,
        tpsOk ? "true" : "false", state.sasRaw, kSasCenterRaw, sasAbsoluteDeg(),
        sasRelative, sasRelative * kSasToSteeringRatio,
        sasFresh ? "true" : "false", (sasFresh || tpsFresh) ? "true" : "false",
        EV_REAR_UART_MODE ? "REAR UART / ESP" : "CAN / ESP",
        state.tvRequested, state.tvApplied, state.regenRequested, state.regenApplied,
        state.tvLimit, state.regenLimit, state.tvRamp, state.regenRamp,
        state.driveMode, state.driverSequence, state.rearSequence, state.configSequence, state.configAck ? "true" : "false",
        driverFresh ? "true" : "false", (state.rearFlags & DJY_REAR_STATUS_TV_ACTIVE) ? "true" : "false",
        regenReady ? "true" : "false", state.regenApplied > 0 ? "true" : "false",
        tvLimited ? "true" : "false", state.regenApplied + 1u < state.regenRequested ? "true" : "false",
        pitAllowed() ? "true" : "false", (!EV_REAR_UART_MODE && driverFresh && rearFresh) ? "true" : "false",
        (EV_REAR_UART_MODE && rearFresh) ? "true" : "false",
        rearFresh ? "true" : "false", state.imuValid ? "true" : "false", state.fault,
        static_cast<unsigned long>(state.rearUartRx), static_cast<unsigned long>(state.rearUartErrors), reason,
        (EV_REAR_UART_MODE && driverFresh) ? "PIT" : "WHEEL",
        (EV_REAR_UART_MODE && rearFresh) ? "true" : "false",
        WiFi.status() == WL_CONNECTED ? "true" : "false",
        WiFi.status() == WL_CONNECTED ? static_cast<long>(WiFi.RSSI()) : -127L,
        EV_RELAY_ENABLED ? "true" : "false",
        static_cast<unsigned long>(relayTx), static_cast<unsigned long>(relayErrors),
        static_cast<unsigned long>(relayCommands), relayHttpStatus, relayDns, relayTcp443, relayTls);

    if (EV_LOCAL_UDP_ENABLED && WiFi.status() == WL_CONNECTED) {
        telemetryUdp.beginPacket(pitHost, EV_TELEMETRY_PORT);
        telemetryUdp.write(reinterpret_cast<const uint8_t*>(json), strlen(json));
        telemetryUdp.endPacket();
    }
#if EV_RELAY_ENABLED
    stageRelayTelemetry(json);
#endif
    Serial.println(json);
    state.configAck = false;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(10);
    Serial.println("# DJY ESP gateway boot");
    runStatusLedSelfTest();
    pinMode(EV_PIT_ENABLE_GPIO, INPUT_PULLUP);

    if (EV_REAR_UART_MODE) {
        rearUart.begin(115200, SERIAL_8N1, EV_REAR_UART_RX_GPIO, EV_REAR_UART_TX_GPIO);
        rearUartReady = true;
        Serial.println("# Rear UART ready: RX GPIO18 / TX GPIO17, 115200 8N1");
    } else {
        const twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
            static_cast<gpio_num_t>(EV_CAN_TX_GPIO), static_cast<gpio_num_t>(EV_CAN_RX_GPIO), TWAI_MODE_NORMAL);
        const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
        const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        if (twai_driver_install(&general, &timing, &filter) == ESP_OK) {
            canReady = twai_start() == ESP_OK;
        }
        Serial.println(canReady ? "# CAN ready" : "# CAN unavailable");
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(EV_WIFI_SSID, EV_WIFI_PASSWORD);
    pitHost.fromString(EV_PIT_HOST);
    commandUdp.begin(EV_COMMAND_PORT);
    setStatusLed(0, 0, 64);
#if EV_RELAY_ENABLED
    xTaskCreatePinnedToCore(relayTask, "ev-https-relay", 12288, nullptr, 0, nullptr, 0);
#endif
    Serial.println("# DJY ESP gateway ready");
}

void loop() {
    if (rearUartReady) receiveRearUart();
    else receiveCan();
    receiveCommands();
    forwardLiveTvCommand();
    sendEspStatus();
    updateStatusLed();
    publishTelemetry();
    delay(1);
}
