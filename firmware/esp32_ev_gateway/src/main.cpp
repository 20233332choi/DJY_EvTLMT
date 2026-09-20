#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <esp32-hal-rgb-led.h>
#include <driver/twai.h>
#include <lwip/sockets.h>
#include <time.h>
#include <esp_system.h>

#include "config.h"
#include "djy_can_protocol.h"
#include "djy_uart_protocol.h"
#include "letsencrypt_gen_y.h"
#include "rear_uart_line.h"
#include "rear_timing.h"
#include "telemetry_queue.h"
#if EV_RELAY_FAST_RSA_TLS
#include "ngrok_tls_client.h"
#endif
#if EV_TLS_DIAGNOSTICS_ONCE
#include "relay_tls_diagnostics.h"
#endif

#ifndef EV_REAR_UART_MODE
#define EV_REAR_UART_MODE 0
#endif

#ifndef EV_REAR_UART_RX_GPIO
#define EV_REAR_UART_RX_GPIO 18
#endif

#ifndef EV_REAR_UART_TX_GPIO
#define EV_REAR_UART_TX_GPIO 17
#endif

#ifndef EV_BMS_CAN_ENABLED
#define EV_BMS_CAN_ENABLED 0
#endif

#ifndef EV_BMS_CAN_TX_GPIO
#define EV_BMS_CAN_TX_GPIO 9
#endif

#ifndef EV_BMS_CAN_RX_GPIO
#define EV_BMS_CAN_RX_GPIO 8
#endif

#if EV_BMS_CAN_ENABLED && !EV_REAR_UART_MODE
#error "DALY BMS CAN shares the ESP TWAI controller; enable it only in Rear UART mode"
#endif

#ifndef EV_LOCAL_UDP_ENABLED
#define EV_LOCAL_UDP_ENABLED 1
#endif

#ifndef EV_RECEIVE_ONLY
#define EV_RECEIVE_ONLY 0
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

int telemetrySocket = -1;
WiFiUDP commandUdp;
IPAddress pitHost;
bool canReady = false;
bool bmsCanReady = false;
HardwareSerial rearUart(1);
bool rearUartReady = false;
uint32_t wifiLastReconnectMs = 0;
uint32_t wifiReconnectAttempts = 0;
constexpr uint16_t kSasCounts = 16384u;
constexpr uint16_t kSasCenterRaw = 8192u;
constexpr float kSasToSteeringRatio = -0.2f;
constexpr size_t kTelemetryJsonSize = 4096u;

struct VehicleState {
    RearTiming timing;
    bool sasValid = false;
    uint16_t sasCenterRaw = 8192;
    float steeringRad = 0.0f;
    uint32_t sequence = 0;
    uint16_t sasRaw = 0, tpsRaw = 0;
    uint16_t rpmLeft = 0, rpmRight = 0;
    uint32_t captureLeft = 0, captureRight = 0;
    uint32_t glitchLeft = 0, glitchRight = 0;
    uint32_t desyncLeft = 0, desyncRight = 0;
    uint32_t stmCanRx = 0, stmCanErrors = 0, stmCanStatus = 0;
    uint32_t stmImuDiag0 = 0, stmImuDiag1 = 0, stmImuDiag2 = 0, stmImuDiag3 = 0;
    uint16_t dacLeft = 0, dacRight = 0;
    uint8_t tvRequested = 0, regenRequested = 0;
    uint8_t tvApplied = 0, regenApplied = 0;
    uint8_t driveMode = 1, controlFlags = 0, rearFlags = 0, fault = 0;
    uint8_t driverSequence = 0, rearSequence = 0, configSequence = 0;
    uint8_t tvLimit = 100, regenLimit = 0;
    uint16_t tvRamp = 200, regenRamp = 0;
    bool configAck = false;
    bool imuValid = false;
    float yawRateRadS = 0.0f, lateralAccelMS2 = 0.0f, longitudinalAccelMS2 = 0.0f;
    float imuRawAx = 0.0f, imuRawAy = 0.0f, imuRawAz = 0.0f;
    float vehicleSpeedMS = 0.0f, desiredYawRadS = 0.0f, yawErrorRadS = 0.0f;
    float deltaPowerKw = 0.0f, powerLeftKw = 0.0f, powerRightKw = 0.0f;
    float tractionScale = 1.0f;
    float pidKp = 0.0f, pidKi = 0.0f, pidKd = 0.0f;
    uint32_t pidMs = 0;
    bool edActive = false;
    uint32_t rearUartRx = 0, rearUartErrors = 0, rearUartBytes = 0;
    uint32_t rearRxHigh = 0, rearRxLow = 0;
    bool rpmLeftValid = false, rpmRightValid = false;
    uint32_t rearCommandRx = 0, rearCommandErrors = 0, rearExtendedMs = 0, tqvInternalMs = 0;
    int16_t tpsReportedPct = -1;
    uint32_t sasMs = 0, tpsMs = 0, driverMs = 0, rearMs = 0;
} state;

struct BmsState {
    float voltage = 0.0f, current = 0.0f, soc = 0.0f;
    float maxCell = 0.0f, minCell = 0.0f, maxTemp = 0.0f, minTemp = 0.0f;
    float remainingAh = 0.0f;
    uint16_t maxCellNumber = 0, minCellNumber = 0, cycleCount = 0;
    uint8_t cellCount = 0, tempCount = 0, life = 0;
    uint8_t state = 0;
    bool chargeMos = false, dischargeMos = false, charger = false, load = false, balancing = false;
    uint8_t alarm[8] = {};
    float cells[48] = {};
    bool cellSeen[48] = {};
    uint32_t lastRxMs = 0;
} bms;

uint8_t bmsRequestId = 0x90;
uint32_t bmsNextRequestMs = 0;

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
TelemetryQueue<kTelemetryJsonSize, 16> relaySamples;
char relayStreamId[17] = {};
constexpr size_t kRelayBatchSize = 4 * kTelemetryJsonSize + 512;
// Task-owned static buffer avoids a 17 KB stack allocation.
char relayBatch[kRelayBatchSize] = {};
char relayCommand[512] = {};
bool relayCommandPending = false;
uint32_t relayTxCount = 0;
uint32_t relayLastSuccessMs = 0;
uint32_t relayErrorCount = 0;
uint32_t relayCommandCount = 0;
int relayLastHttpStatus = 0;
int relayDnsStatus = 0;
int relayTcp443Status = 0;
int relayTlsError = 0;

void stageRelayTelemetry(const char* json) {
    portENTER_CRITICAL(&relayMux);
    relaySamples.push(state.sequence, json);
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
    String relayUrl(EV_RELAY_URL);
    /* Prefer TLS even for telemetry. Existing configs may still contain an
     * http:// ngrok URL from the original read-only implementation. */
    if (relayUrl.startsWith("http://") && !EV_ALLOW_PLAINTEXT_RELAY) {
        relayUrl = String("https://") + relayUrl.substring(7);
    }
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

    snprintf(relayStreamId, sizeof(relayStreamId), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    uint32_t retryMs = 1000;
    bool networkPathChecked = false;
    bool networkTimeRequested = false;
#if EV_TLS_DIAGNOSTICS_ONCE
    bool tlsProbed = false;
#endif
    // Keep the authenticated TLS socket alive across telemetry exchanges.
    WiFiClient plainClient;
#if EV_RELAY_FAST_RSA_TLS
    NgrokTlsClient secureClient;
#else
    WiFiClientSecure secureClient;
#endif
    static const char* relayAlpn[] = {"http/1.1", nullptr};
    secureClient.setAlpnProtocols(relayAlpn);
    WiFiClient* relayClient = relayUsesTls ? static_cast<WiFiClient*>(&secureClient) : &plainClient;
#if defined(EV_RELAY_CA_CERT)
    secureClient.setCACert(EV_RELAY_CA_CERT);
#elif EV_RELAY_ALLOW_INSECURE_TLS
    secureClient.setInsecure();
#else
    secureClient.setCACert(kLetsEncryptGenYRoots);
#endif
    secureClient.setHandshakeTimeout(5);
    HTTPClient http;
    http.useHTTP10(false);
    http.setReuse(true);
    http.setConnectTimeout(5000);
    http.setTimeout(2500);
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            relayClient->stop();
            networkPathChecked = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* X.509 validation is meaningless while the ESP still thinks it is
         * 1970. NTP runs in the networking stack; only this low-priority relay
         * task waits, while UART and vehicle telemetry continue normally. */
        if (relayUsesTls && time(nullptr) < 1700000000) {
            if (!networkTimeRequested) {
                configTime(0, 0, "pool.ntp.org", "time.google.com");
                networkTimeRequested = true;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

#if EV_TLS_DIAGNOSTICS_ONCE
        if (!tlsProbed) {
            // Delay permits a serial observer to attach after an upload reset.
            vTaskDelay(pdMS_TO_TICKS(8000));
            for (unsigned profile = 3; profile < 5; ++profile) {
                RelayTlsProbe probe;
                probe.run(relayAuthority.c_str(), relayPort, kLetsEncryptGenYRoots, profile);
            }
            tlsProbed = true;
        }
#endif
        if (!networkPathChecked) {
            IPAddress relayAddress;
            const int dnsOk = WiFi.hostByName(relayAuthority.c_str(), relayAddress) == 1 ? 1 : -1;
            int tcpOk = 0;
            if (dnsOk == 1) {
                Serial.printf("# Relay resolved=%s epoch=%ld free_heap=%u\n",
                              relayAddress.toString().c_str(), static_cast<long>(time(nullptr)),
                              static_cast<unsigned>(ESP.getFreeHeap()));
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

        uint32_t lastSequence = 0;
        uint32_t droppedSamples = 0;
        size_t queuedSamples = 0;
        portENTER_CRITICAL(&relayMux);
        droppedSamples = relaySamples.dropped();
        const int prefix = snprintf(relayBatch, sizeof(relayBatch),
            "{\"stream_id\":\"%s\",\"sent_ms\":%lu,\"dropped_samples\":%lu,\"samples\":",
            relayStreamId, static_cast<unsigned long>(millis()), static_cast<unsigned long>(droppedSamples));
        queuedSamples = relaySamples.batch(relayBatch + prefix, sizeof(relayBatch) - prefix - 1, 4, lastSequence);
        portEXIT_CRITICAL(&relayMux);
        if (queuedSamples == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        strlcat(relayBatch, "}", sizeof(relayBatch));

        bool success = false;
        if (http.begin(*relayClient, relayUrl)) {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + relayToken);
            http.addHeader("X-Vehicle-ID", EV_VEHICLE_ID);
            http.addHeader("ngrok-skip-browser-warning", "1");
            const int status = http.POST(reinterpret_cast<uint8_t*>(relayBatch), strlen(relayBatch));
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
                const int ackStart = response.indexOf("\"ack_seq\"");
                const int ackColon = ackStart >= 0 ? response.indexOf(':', ackStart) : -1;
                const uint32_t ack = ackColon >= 0 ? strtoul(response.c_str() + ackColon + 1, nullptr, 10) : 0;
                if (ack == lastSequence) {
                    stageRelayCommand(response);
                    portENTER_CRITICAL(&relayMux);
                    relaySamples.acknowledge(ack);
                    ++relayTxCount;
                    relayLastSuccessMs = millis();
                    portEXIT_CRITICAL(&relayMux);
                    retryMs = 1000;
                    success = true;
                }
            }
            http.end();
        } else {
            portENTER_CRITICAL(&relayMux);
            relayLastHttpStatus = -1000;
            portEXIT_CRITICAL(&relayMux);
        }
        if (!success) {
            relayClient->stop();
            portENTER_CRITICAL(&relayMux);
            ++relayErrorCount;
            portEXIT_CRITICAL(&relayMux);
            vTaskDelay(pdMS_TO_TICKS(retryMs));
            retryMs = min<uint32_t>(retryMs * 2u, 5000u);
        } else {
            // Bound request rate; keep-alive saves TLS work, not HTTP quota.
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

void maintainWifi() {
    static uint32_t lastReport = 0;
    const uint32_t reportNow = millis();
    if (reportNow - lastReport >= 2000u) {
        lastReport = reportNow;
        Serial.printf("# WiFi status=%d ip=%s rssi=%ld reconnects=%lu\n",
                      static_cast<int>(WiFi.status()), WiFi.localIP().toString().c_str(),
                      WiFi.status() == WL_CONNECTED ? static_cast<long>(WiFi.RSSI()) : -127L,
                      static_cast<unsigned long>(wifiReconnectAttempts));
    }
    if (WiFi.status() == WL_CONNECTED) return;
    const uint32_t now = millis();
    if (now - wifiLastReconnectMs < 15000u) return;
    wifiLastReconnectMs = now;
    ++wifiReconnectAttempts;
    if (!WiFi.reconnect()) {
        WiFi.begin(EV_WIFI_SSID, EV_WIFI_PASSWORD);
    }
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
    uint32_t relayTx = 0, relaySuccessMs = 0;
    portENTER_CRITICAL(&relayMux);
    relayTx = relayTxCount;
    relaySuccessMs = relayLastSuccessMs;
    portEXIT_CRITICAL(&relayMux);
    if (relayTx > 0u && now - relaySuccessMs < 5000u) {
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
    // User-confirmed tire outside diameter 45 cm and reduction 4:1.
    // Keep STM vehicleSpeedMS separately as its original control input.
    constexpr float kGearRatio = 4.0f, kTireRadiusM = 0.225f;
    // A single noisy/missing wheel must never become vehicle speed.
    if ((state.rpmLeft == 0u) != (state.rpmRight == 0u)) return 0.0f;
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

uint16_t bmsU16(const uint8_t* data, uint8_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

uint32_t bmsU32(const uint8_t* data, uint8_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

void receiveBmsCan() {
#if EV_BMS_CAN_ENABLED
    twai_message_t frame = {};
    while (twai_receive(&frame, 0) == ESP_OK) {
        if (!frame.extd || frame.rtr || frame.data_length_code != 8) continue;
        const uint8_t id = static_cast<uint8_t>((frame.identifier >> 16) & 0xffu);
        if (id < 0x90 || id > 0x98 ||
            frame.identifier != (0x18000000u | (static_cast<uint32_t>(id) << 16) | 0x4001u)) continue;
        const uint8_t* d = frame.data;
        bms.lastRxMs = millis();
        if (id == 0x90) {
            bms.voltage = bmsU16(d, 0) / 10.0f;
            bms.current = (static_cast<int32_t>(bmsU16(d, 4)) - 30000) / 10.0f;
            bms.soc = bmsU16(d, 6) / 10.0f;
        } else if (id == 0x91) {
            bms.maxCell = bmsU16(d, 0) / 1000.0f; bms.maxCellNumber = d[2];
            bms.minCell = bmsU16(d, 3) / 1000.0f; bms.minCellNumber = d[5];
        } else if (id == 0x92) {
            bms.maxTemp = static_cast<float>(d[0]) - 40.0f;
            bms.minTemp = static_cast<float>(d[2]) - 40.0f;
        } else if (id == 0x93) {
            bms.state = d[0]; bms.chargeMos = d[1] != 0; bms.dischargeMos = d[2] != 0;
            bms.life = d[3]; bms.remainingAh = bmsU32(d, 4) / 1000.0f;
        } else if (id == 0x94) {
            bms.cellCount = d[0] > 48 ? 48 : d[0]; bms.tempCount = d[1] > 16 ? 16 : d[1];
            bms.charger = d[2] != 0; bms.load = d[3] != 0; bms.cycleCount = bmsU16(d, 6);
        } else if (id == 0x95) {
            if (d[0] != 0) {
                const uint8_t firstCell = static_cast<uint8_t>((d[0] - 1u) * 3u);
                for (uint8_t i = 0; i < 3 && firstCell + i < 48; ++i) {
                    const uint8_t cell = static_cast<uint8_t>(firstCell + i);
                    bms.cells[cell] = bmsU16(d, static_cast<uint8_t>(1u + i * 2u)) / 1000.0f;
                    bms.cellSeen[cell] = bms.cells[cell] > 0.0f;
                }
            }
        } else if (id == 0x97) {
            bms.balancing = false;
            for (uint8_t i = 0; i < 8; ++i) bms.balancing = bms.balancing || d[i] != 0;
        } else if (id == 0x98) {
            memcpy(bms.alarm, d, sizeof(bms.alarm));
        }
    }
    const uint32_t now = millis();
    if (bmsCanReady && static_cast<int32_t>(now - bmsNextRequestMs) >= 0) {
        twai_message_t request = {};
        request.identifier = 0x18000000u | (static_cast<uint32_t>(bmsRequestId) << 16) | 0x0140u;
        request.extd = 1;
        request.data_length_code = 8;
        if (twai_transmit(&request, 0) == ESP_OK) {
            bmsRequestId = bmsRequestId == 0x98 ? 0x90 : static_cast<uint8_t>(bmsRequestId + 1u);
            bmsNextRequestMs = now + 100u;
        } else {
            bmsNextRequestMs = now + 20u;
        }
    }
#endif
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

bool parseRearUartLine(const char* line) {
    unsigned rpmLeft = 0, rpmRight = 0, tpsRaw = 0, idleRaw = 0;
    unsigned espFresh = 0, espSequence = 0;
    unsigned imuValid = 0, sasRaw = 0;
    long yawMilli = 0, latMilli = 0, lonMilli = 0;
    long axMilli = 0, ayMilli = 0, azMilli = 0;
    unsigned long captureLeft = 0, captureRight = 0;
    unsigned long glitchLeft = 0, glitchRight = 0, espRxCount = 0;
    int tpsPct = 0;
    const int headFields = sscanf(
        line,
        "L=%u R=%u cap=%lu/%lu glt=%lu/%lu",
        &rpmLeft, &rpmRight, &captureLeft, &captureRight, &glitchLeft, &glitchRight);
    // Team firmware can insert diagnostic fields between glt and tps. Locate
    // the stable TPS marker instead of requiring one monolithic sentence.
    const char* tps = strstr(line, " tps=");
    const int tpsFields = tps == nullptr ? 0 :
        sscanf(tps, " tps=%u pct=%d idle=%u", &tpsRaw, &tpsPct, &idleRaw);
    const char* esp = strstr(line, " esp=");
    if (esp != nullptr) {
        (void)sscanf(esp, " esp=%u/%u rx=%lu", &espFresh, &espSequence, &espRxCount);
    }
    if (headFields != 6 || tpsFields != 3 || rpmLeft > 65535u || rpmRight > 65535u ||
        tpsRaw > 4095u || idleRaw > 4095u || tpsPct < 0 || tpsPct > 100) return false;
    state.timing.parse(line); // Missing/malformed extension clears previous timing.

    unsigned validLeft = 0, validRight = 0;
    const char* validity = strstr(line, " rv=");
    if (validity != nullptr && (sscanf(validity, " rv=%u/%u", &validLeft, &validRight) != 2 ||
                               validLeft > 1u || validRight > 1u)) return false;
    // Missing validity is unverified, not a valid stationary measurement.
    state.rpmLeftValid = validity != nullptr && validLeft == 1u;
    state.rpmRightValid = validity != nullptr && validRight == 1u;

    // A stopped motor can leave an un-driven capture input floating. If almost
    // every capture was rejected as a glitch, never present that RPM as real.
    const bool leftSignalInvalid = glitchLeft > captureLeft + 100u;
    const bool rightSignalInvalid = glitchRight > captureRight + 100u;
    (void)idleRaw;
    (void)espFresh;
    (void)espSequence;
    (void)espRxCount;

    state.rpmLeft = leftSignalInvalid ? 0u : static_cast<uint16_t>(constrain(rpmLeft, 0u, 65535u));
    state.rpmRight = rightSignalInvalid ? 0u : static_cast<uint16_t>(constrain(rpmRight, 0u, 65535u));
    state.captureLeft = static_cast<uint32_t>(captureLeft);
    state.captureRight = static_cast<uint32_t>(captureRight);
    state.glitchLeft = static_cast<uint32_t>(glitchLeft);
    state.glitchRight = static_cast<uint32_t>(glitchRight);
    state.tpsRaw = static_cast<uint16_t>(constrain(tpsRaw, 0u, 65535u));
    state.tpsReportedPct = static_cast<int16_t>(constrain(tpsPct, 0, 100));
    state.rearSequence++;
    const uint32_t now = millis();
    state.tpsMs = now;
    state.rearMs = now;

    unsigned long diag0 = 0, diag1 = 0, diag2 = 0, diag3 = 0;
    const char* desync = strstr(line, " dsy=");
    if (desync != nullptr && sscanf(desync, " dsy=%lu/%lu", &diag0, &diag1) == 2) {
        state.desyncLeft = static_cast<uint32_t>(diag0);
        state.desyncRight = static_cast<uint32_t>(diag1);
    }
    const char* canDiag = strstr(line, " can=");
    if (canDiag != nullptr && sscanf(canDiag, " can=%lu/%lu/%lx", &diag0, &diag1, &diag2) == 3) {
        state.stmCanRx = static_cast<uint32_t>(diag0);
        state.stmCanErrors = static_cast<uint32_t>(diag1);
        state.stmCanStatus = static_cast<uint32_t>(diag2);
    }

    const char* sensor = strstr(line, " imu=");
    const int sensorFields = sensor == nullptr ? 0 :
        sscanf(sensor, " imu=%u sas=%u yaw=%ld lat=%ld lon=%ld ax=%ld ay=%ld az=%ld",
               &imuValid, &sasRaw, &yawMilli, &latMilli, &lonMilli,
               &axMilli, &ayMilli, &azMilli);
    if (sensor != nullptr && sensorFields >= 4 &&
        imuValid <= 1u && sasRaw <= 16383u) {
        state.imuValid = imuValid != 0u;
        state.sasRaw = static_cast<uint16_t>(sasRaw);
        state.yawRateRadS = static_cast<float>(yawMilli) / 1000.0f;
        state.lateralAccelMS2 = static_cast<float>(latMilli) / 1000.0f;
        state.longitudinalAccelMS2 = static_cast<float>(lonMilli) / 1000.0f;
        state.imuRawAx = static_cast<float>(axMilli) / 1000.0f;
        state.imuRawAy = static_cast<float>(ayMilli) / 1000.0f;
        state.imuRawAz = static_cast<float>(azMilli) / 1000.0f;
        state.sasMs = now;
    } else {
        state.imuValid = false;
        state.sasMs = 0;
        state.yawRateRadS = 0.0f;
        state.lateralAccelMS2 = 0.0f;
        state.longitudinalAccelMS2 = 0.0f;
        state.imuRawAx = 0.0f;
        state.imuRawAy = 0.0f;
        state.imuRawAz = 0.0f;
    }
    if (sensor != nullptr && sscanf(sensor, " imu=%lu/%lu/%lu/%lu",
                                   &diag0, &diag1, &diag2, &diag3) == 4) {
        state.stmImuDiag0 = static_cast<uint32_t>(diag0);
        state.stmImuDiag1 = static_cast<uint32_t>(diag1);
        state.stmImuDiag2 = static_cast<uint32_t>(diag2);
        state.stmImuDiag3 = static_cast<uint32_t>(diag3);
    }
    const char* imuDiag = strstr(line, " idg=");
    if (imuDiag != nullptr && sscanf(imuDiag, " idg=%lu/%lu/%lu/%lu", &diag0, &diag1, &diag2, &diag3) == 4) {
        state.stmImuDiag0 = static_cast<uint32_t>(diag0);
        state.stmImuDiag1 = static_cast<uint32_t>(diag1);
        state.stmImuDiag2 = static_cast<uint32_t>(diag2);
        state.stmImuDiag3 = static_cast<uint32_t>(diag3);
    }

    long vehicleSpeedMilli = 0, desiredYawMilli = 0, yawErrorMilli = 0;
    long deltaPowerMilli = 0, powerLeftMilli = 0, powerRightMilli = 0, tractionMilli = 1000;
    unsigned tqvActive = 0, tqvEdActive = 0;
    const char* tqv = strstr(line, " vs=");
    const bool tqvExtended = tqv != nullptr && sscanf(
        tqv,
        " vs=%ld dy=%ld ye=%ld dp=%ld pl=%ld pr=%ld tva=%u eda=%u tr=%ld",
        &vehicleSpeedMilli, &desiredYawMilli, &yawErrorMilli, &deltaPowerMilli,
        &powerLeftMilli, &powerRightMilli, &tqvActive, &tqvEdActive, &tractionMilli) == 9 &&
        tqvActive <= 1u && tqvEdActive <= 1u && tractionMilli >= 0 && tractionMilli <= 1000;
    if (tqvExtended) {
        state.vehicleSpeedMS = static_cast<float>(vehicleSpeedMilli) / 1000.0f;
        state.desiredYawRadS = static_cast<float>(desiredYawMilli) / 1000.0f;
        state.yawErrorRadS = static_cast<float>(yawErrorMilli) / 1000.0f;
        state.deltaPowerKw = static_cast<float>(deltaPowerMilli) / 1000.0f;
        state.powerLeftKw = static_cast<float>(powerLeftMilli) / 1000.0f;
        state.powerRightKw = static_cast<float>(powerRightMilli) / 1000.0f;
        state.tractionScale = static_cast<float>(tractionMilli) / 1000.0f;
        state.edActive = tqvEdActive != 0u;
        if (tqvActive) state.rearFlags |= DJY_REAR_STATUS_TV_ACTIVE;
        else state.rearFlags &= static_cast<uint8_t>(~DJY_REAR_STATUS_TV_ACTIVE);
        if (state.edActive) state.rearFlags |= DJY_REAR_STATUS_ED_ACTIVE;
        else state.rearFlags &= static_cast<uint8_t>(~DJY_REAR_STATUS_ED_ACTIVE);
        state.rearExtendedMs = now;
        state.tqvInternalMs = now;
    } else {
        // A legacy/team sentence without these fields is an explicit
        // "not received" sample, not a new sample containing zeroes.
        state.vehicleSpeedMS = 0.0f;
        state.desiredYawRadS = 0.0f;
        state.yawErrorRadS = 0.0f;
        state.deltaPowerKw = 0.0f;
        state.powerLeftKw = 0.0f;
        state.powerRightKw = 0.0f;
        state.tractionScale = 1.0f;
        state.edActive = false;
        state.tqvInternalMs = 0u;
    }

    long kp = 0, ki = 0, kd = 0;
    const char* pid = strstr(line, " kp=");
    state.pidMs = 0;
    if (pid != nullptr && sscanf(pid, " kp=%ld ki=%ld kd=%ld", &kp, &ki, &kd) == 3 &&
        kp >= 0 && ki >= 0 && kd >= 0) {
        state.pidKp = kp / 1000.0f;
        state.pidKi = ki / 1000.0f;
        state.pidKd = kd / 1000.0f;
        state.pidMs = now;
    }

    unsigned controlFresh = 0, controlSequence = 0, requested = 0, limit = 100, applied = 0;
    unsigned tvActive = 0, edActive = 0, fault = 0, dacLeft = 0, dacRight = 0;
    // Optional IMU/SAS fields are declared above with the base fields.
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
        // These are commands received by STM, not telemetry received by ESP.
        state.rearCommandRx = static_cast<uint32_t>(uartRx);
        state.rearCommandErrors = static_cast<uint32_t>(uartErrors);
        state.rearExtendedMs = now;
    } else {
        // Legacy firmware reports RPM/TPS only; do not retain old optional data.
        if (!tqvExtended) state.rearExtendedMs = 0;
        state.driverMs = 0;
        state.configAck = false;
    }
    long steeringMilli = 0;
    unsigned sasValid = 0, sasCenter = 0;
    const char* steering = strstr(line, " steer=");
    state.sasValid = false;
    state.steeringRad = 0.0f;
    if (steering != nullptr && sscanf(steering, " steer=%ld sv=%u sc=%u",
            &steeringMilli, &sasValid, &sasCenter) == 3 &&
            steeringMilli >= -3142 && steeringMilli <= 3142 && sasValid <= 1u && sasCenter < 16384u) {
        state.sasValid = sasValid != 0u;
        state.sasCenterRaw = static_cast<uint16_t>(sasCenter);
        state.steeringRad = static_cast<float>(steeringMilli) / 1000.0f;
    }
    ++state.rearUartRx;
    return true;
}

bool rearSamplePending = false;
void publishTelemetry();

void receiveRearUart() {
    static RearUartLine line;
    if (digitalRead(EV_REAR_UART_RX_GPIO) == HIGH) ++state.rearRxHigh;
    else ++state.rearRxLow;
    while (rearUart.available()) {
        ++state.rearUartBytes;
        const auto result = line.push(static_cast<char>(rearUart.read()));
        if (result == RearUartLine::Dropped) {
            ++state.rearUartErrors;
        } else if (result == RearUartLine::Ready && line.data()[0] != '\0') {
            if (!parseRearUartLine(line.data())) ++state.rearUartErrors;
            else {
                // Snapshot each complete Rear record before parsing the next.
                rearSamplePending = true;
                publishTelemetry();
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
    if (EV_RECEIVE_ONLY) return;
    static uint32_t previous = 0u;
    const uint32_t now = millis();
    if (!rearUartReady || !liveCommandSeen || now - liveCommandMs > 500u ||
        state.rearExtendedMs == 0u || now - state.rearExtendedMs >= 300u ||
        now - previous < 100u) {
        return;
    }
    previous = now;
    uint8_t frame[DJY_UART_LIVE_TV_SIZE];
    djy_uart_pack_live_tv(frame, &liveCommand);
    rearUart.write(frame, sizeof(frame));
}

void handleCommand(const String& input) {
    if (EV_RECEIVE_ONLY) return;
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
    // Every Rear record is published immediately. With no Rear input, retain
    // a 1 Hz heartbeat/BMS update; never poll-and-overwrite multiple records.
    if (!rearSamplePending && millis() - previous < (EV_REAR_UART_MODE ? 1000u : 200u)) return;
    rearSamplePending = false;
    previous = millis();
    ++state.sequence;
    const bool driverFresh = state.driverMs != 0u && millis() - state.driverMs < 200u;
    const bool rearFresh = state.rearMs != 0u && millis() - state.rearMs < 300u;
    const bool rearOutputFresh = rearFresh && state.rearExtendedMs != 0u &&
                                 millis() - state.rearExtendedMs < 300u;
    const bool tqvInternalFresh = rearFresh && state.tqvInternalMs != 0u &&
                                  millis() - state.tqvInternalMs < 300u;
    const bool sasFresh = state.sasMs != 0u && millis() - state.sasMs < 200u;
    const bool tpsFresh = state.tpsMs != 0u && millis() - state.tpsMs < 300u;
    // tv_stm_esp: TPS_ADC_MIN/MAX=885/2820, margin=200.
    const bool tpsOk = tpsFresh && state.tpsRaw >= 685u && state.tpsRaw <= 3020u;
    const bool frontFresh = rearOutputFresh && state.fault != 1u;
    const bool rpmLeftOk = rearFresh && (EV_REAR_UART_MODE ? state.rpmLeftValid : state.rpmLeft != 0u);
    const bool rpmRightOk = rearFresh && (EV_REAR_UART_MODE ? state.rpmRightValid : state.rpmRight != 0u);
    const char* rpmLeftQuality = !rearFresh ? "NO_DATA" : rpmLeftOk ? "OK" :
        state.captureLeft == 0u ? "NO_PULSES" : state.glitchLeft > 0u ? "NOISY" : "UNVERIFIED";
    const char* rpmRightQuality = !rearFresh ? "NO_DATA" : rpmRightOk ? "OK" :
        state.captureRight == 0u ? "NO_PULSES" : state.glitchRight > 0u ? "NOISY" : "UNVERIFIED";
    const float sasRelative = sasFresh ? sasRelativeDeg() : 0.0f;
    const bool tvLimited = state.tvApplied + 1u < state.tvRequested;
    const bool regenReady = (state.rearFlags & DJY_REAR_STATUS_REGEN_READY) != 0;
    const char* reason = EV_REAR_UART_MODE && EV_RECEIVE_ONLY ?
                         (!rearFresh ? "REAR_STATUS_TIMEOUT" : state.fault ? "STM_FAULT" : "NONE") :
                         EV_REAR_UART_MODE && !driverFresh ? "PIT_CONTROL_TIMEOUT" :
                         !driverFresh ? "DRIVER_CONTROL_TIMEOUT" :
                         !rearFresh ? "REAR_STATUS_TIMEOUT" :
                         (!regenReady && state.regenRequested > 0) ? "REGEN_INTERFACE_UNVALIDATED" :
                         tvLimited ? "PIT_LIMIT_OR_RAMP" : "NONE";

    const bool bmsOnline = bms.lastRxMs != 0u && millis() - bms.lastRxMs < 2000u;
    bool bmsFault = false;
    char alarmHex[17] = {};
    char cellsJson[512] = "[";
    size_t cellsLength = 1;
    bool firstCell = true;
    for (uint8_t i = 0; i < 8; ++i) {
        bmsFault = bmsFault || bms.alarm[i] != 0;
        snprintf(alarmHex + i * 2u, sizeof(alarmHex) - i * 2u, "%02X", bms.alarm[i]);
    }
    for (uint8_t i = 0; i < bms.cellCount; ++i) {
        if (!bms.cellSeen[i]) continue;
        const int written = snprintf(cellsJson + cellsLength, sizeof(cellsJson) - cellsLength,
                                     "%s%.3f", firstCell ? "" : ",", bms.cells[i]);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(cellsJson) - cellsLength) break;
        cellsLength += static_cast<size_t>(written);
        firstCell = false;
    }
    strlcat(cellsJson, "]", sizeof(cellsJson));
    const char* bmsState = bms.state == 1 ? "CHARGING" : bms.state == 2 ? "DISCHARGING" :
                           bms.state == 0 ? "STANDBY" : "UNKNOWN";

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
    char json[kTelemetryJsonSize];
    char timingJson[256];
    if(!state.timing.json(timingJson,sizeof(timingJson)))return;
    int jsonLength = snprintf(json, sizeof(json),
        "{\"board_timing\":%s,\"seq\":%lu,\"timestamp_ms\":%lu,\"rpm_left\":%u,\"rpm_right\":%u,"
        "\"rpm_left_reported\":%u,\"rpm_right_reported\":%u,\"rpm_left_reported_online\":%s,\"rpm_right_reported_online\":%s,"
        "\"rpm_left_quality\":\"%s\",\"rpm_right_quality\":\"%s\","
        "\"cap_left\":%lu,\"cap_right\":%lu,\"rpm_glitch_left\":%lu,\"rpm_glitch_right\":%lu,"
        "\"rear_desync_left\":%lu,\"rear_desync_right\":%lu,"
        "\"rpm_left_online\":%s,\"rpm_right_online\":%s,\"motor_left_ok\":%s,\"motor_right_ok\":%s,"
        "\"speed_kmh\":%.2f,\"speed_online\":%s,\"tps_raw\":%u,\"tps_pct\":%.2f,\"tps_online\":%s,\"tps_ok\":%s,\"dac_left\":%u,\"dac_right\":%u,"
        "\"sas_raw\":%u,\"sas_center_raw\":%u,\"sas_absolute_deg\":%.3f,"
        "\"sas_relative_deg\":%.3f,\"sas_deg\":%.3f,\"sas_online\":%s,\"sas_ok\":%s,\"front_sensor_online\":%s,\"front_source\":\"FRONT CAN / REAR UART\","
        "\"tv_requested_pct\":%u,\"tv_applied_pct\":%u,\"regen_requested_pct\":%u,\"regen_applied_pct\":%u,"
        "\"tv_limit_pct\":%u,\"regen_limit_pct\":%u,\"tv_ramp_pct_s\":%u,\"regen_ramp_pct_s\":%u,"
        "\"drive_mode\":%u,\"driver_control_seq\":%u,\"rear_status_seq\":%u,\"config_seq\":%u,\"config_ack\":%s,"
        "\"driver_control_fresh\":%s,\"tv_active\":%s,\"ed_active\":%s,\"regen_ready\":%s,\"regen_active\":%s,"
        "\"tv_limited\":%s,\"regen_limited\":%s,\"pit_adjust_allowed\":%s,"
        "\"can_ok\":%s,\"uart_ok\":%s,\"stm_online\":%s,\"imu_ok\":%s,\"imu_online\":%s,\"yaw_rate_rad_s\":%.3f,\"lateral_accel_m_s2\":%.3f,\"longitudinal_accel_m_s2\":%.3f,\"imu_raw_ax_m_s2\":%.3f,\"imu_raw_ay_m_s2\":%.3f,\"imu_raw_az_m_s2\":%.3f,"
        "\"rear_output_online\":%s,\"tqv_internal_online\":%s,"
        "\"pid_kp\":%.3f,\"pid_ki\":%.3f,\"pid_kd\":%.3f,\"pid_online\":%s,"
        "\"vehicle_speed_m_s\":%.3f,\"desired_yaw_rad_s\":%.3f,\"yaw_error_rad_s\":%.3f,\"delta_power_kw\":%.3f,\"power_left_kw\":%.3f,\"power_right_kw\":%.3f,\"traction_scale\":%.3f,\"fault_code\":%u,"
        "\"rear_uart_rx\":%lu,\"rear_uart_errors\":%lu,\"rear_uart_bytes\":%lu,\"rear_rx_high\":%lu,\"rear_rx_low\":%lu,\"rear_command_rx\":%lu,\"rear_command_errors\":%lu,"
        "\"stm_can_rx\":%lu,\"stm_can_errors\":%lu,\"stm_can_status\":%lu,"
        "\"stm_imu_diag_0\":%lu,\"stm_imu_diag_1\":%lu,\"stm_imu_diag_2\":%lu,\"stm_imu_diag_3\":%lu,\"limit_reason\":\"%s\","
        "\"command_source\":\"%s\",\"live_control_allowed\":%s,"
        "\"wifi_connected\":%s,\"wifi_status_code\":%d,\"wifi_reconnect_attempts\":%lu,\"wifi_rssi_dbm\":%ld,\"internet_relay_enabled\":%s,"
        "\"internet_relay_tx\":%lu,\"internet_relay_errors\":%lu,\"internet_relay_commands\":%lu,"
        "\"internet_relay_last_http_status\":%d,\"internet_relay_dns_ok\":%d,"
        "\"internet_relay_tcp443_ok\":%d,\"internet_relay_tls_error\":%d}",
        timingJson,static_cast<unsigned long>(state.sequence), static_cast<unsigned long>(millis()),
        state.rpmLeft, state.rpmRight,
        state.rpmLeft, state.rpmRight, rearFresh ? "true" : "false", rearFresh ? "true" : "false",
        rpmLeftQuality, rpmRightQuality,
        static_cast<unsigned long>(state.captureLeft), static_cast<unsigned long>(state.captureRight),
        static_cast<unsigned long>(state.glitchLeft), static_cast<unsigned long>(state.glitchRight),
        static_cast<unsigned long>(state.desyncLeft), static_cast<unsigned long>(state.desyncRight),
        rpmLeftOk ? "true" : "false", rpmRightOk ? "true" : "false",
        rpmLeftOk ? "true" : "false", rpmRightOk ? "true" : "false",
        speedKmh(), (rpmLeftOk && rpmRightOk && (!EV_REAR_UART_MODE || tqvInternalFresh)) ? "true" : "false",
        state.tpsRaw, throttlePercent(),
        (tpsFresh && frontFresh) ? "true" : "false",
        (tpsOk && frontFresh && state.fault != 2u) ? "true" : "false",
        state.dacLeft, state.dacRight,
        state.sasRaw, state.sasCenterRaw, state.sasRaw * (360.0f / 16384.0f),
        (static_cast<int>(state.sasRaw) - state.sasCenterRaw) * (360.0f / 16384.0f),
        state.steeringRad * (180.0f / PI),
        (rearFresh && frontFresh && state.sasValid) ? "true" : "false",
        (rearFresh && frontFresh && state.sasValid) ? "true" : "false",
        frontFresh ? "true" : "false",
        state.tvRequested, state.tvApplied, state.regenRequested, state.regenApplied,
        state.tvLimit, state.regenLimit, state.tvRamp, state.regenRamp,
        state.driveMode, state.driverSequence, state.rearSequence, state.configSequence, state.configAck ? "true" : "false",
        driverFresh ? "true" : "false", (state.rearFlags & DJY_REAR_STATUS_TV_ACTIVE) ? "true" : "false",
        state.edActive ? "true" : "false", regenReady ? "true" : "false", state.regenApplied > 0 ? "true" : "false",
        tvLimited ? "true" : "false", state.regenApplied + 1u < state.regenRequested ? "true" : "false",
        pitAllowed() ? "true" : "false", (!EV_REAR_UART_MODE && driverFresh && rearFresh) ? "true" : "false",
        (EV_REAR_UART_MODE && rearFresh) ? "true" : "false",
        rearFresh ? "true" : "false", (rearFresh && state.imuValid) ? "true" : "false", (rearFresh && state.imuValid) ? "true" : "false",
        state.yawRateRadS, state.lateralAccelMS2, state.longitudinalAccelMS2,
        state.imuRawAx, state.imuRawAy, state.imuRawAz,
        rearOutputFresh ? "true" : "false", tqvInternalFresh ? "true" : "false",
        state.pidKp, state.pidKi, state.pidKd,
        (rearFresh && state.pidMs != 0u && millis() - state.pidMs < 300u) ? "true" : "false",
        state.vehicleSpeedMS, state.desiredYawRadS, state.yawErrorRadS,
        state.deltaPowerKw, state.powerLeftKw, state.powerRightKw, state.tractionScale, state.fault,
        static_cast<unsigned long>(state.rearUartRx), static_cast<unsigned long>(state.rearUartErrors),
        static_cast<unsigned long>(state.rearUartBytes),
        static_cast<unsigned long>(state.rearRxHigh), static_cast<unsigned long>(state.rearRxLow),
        static_cast<unsigned long>(state.rearCommandRx), static_cast<unsigned long>(state.rearCommandErrors),
        static_cast<unsigned long>(state.stmCanRx), static_cast<unsigned long>(state.stmCanErrors),
        static_cast<unsigned long>(state.stmCanStatus),
        static_cast<unsigned long>(state.stmImuDiag0), static_cast<unsigned long>(state.stmImuDiag1),
        static_cast<unsigned long>(state.stmImuDiag2), static_cast<unsigned long>(state.stmImuDiag3), reason,
        (EV_REAR_UART_MODE && driverFresh) ? "PIT" : "WHEEL",
        (!EV_RECEIVE_ONLY && EV_REAR_UART_MODE && rearFresh && state.rearExtendedMs != 0u &&
         millis() - state.rearExtendedMs < 300u) ? "true" : "false",
        WiFi.status() == WL_CONNECTED ? "true" : "false",
        static_cast<int>(WiFi.status()), static_cast<unsigned long>(wifiReconnectAttempts),
        WiFi.status() == WL_CONNECTED ? static_cast<long>(WiFi.RSSI()) : -127L,
        EV_RELAY_ENABLED ? "true" : "false",
        static_cast<unsigned long>(relayTx), static_cast<unsigned long>(relayErrors),
        static_cast<unsigned long>(relayCommands), relayHttpStatus, relayDns, relayTcp443, relayTls);
    if (jsonLength < 0 || static_cast<size_t>(jsonLength) >= sizeof(json)) return;
    const int bmsJsonLength = snprintf(json + jsonLength - 1, sizeof(json) - static_cast<size_t>(jsonLength) + 1,
        ",\"bms_online\":%s,\"bms_ok\":%s,\"bms_fault\":%s,\"bms_source\":\"ESP CAN\","
        "\"bms_model\":\"DALY R24TS\",\"bms_protocol\":\"DALY CAN 250k\",\"bms_state\":\"%s\","
        "\"bms_age_ms\":%lu,\"battery_pack_voltage_v\":%.1f,\"battery_current_a\":%.1f,"
        "\"battery_power_kw\":%.3f,\"battery_soc_pct\":%.1f,\"bms_life_pct\":%u,"
        "\"bms_remaining_capacity_ah\":%.3f,\"bms_cell_count\":%u,\"bms_temp_count\":%u,"
        "\"bms_max_cell_voltage_v\":%.3f,\"bms_min_cell_voltage_v\":%.3f,"
        "\"bms_max_cell_number\":%u,\"bms_min_cell_number\":%u,\"bms_cell_delta_mv\":%.1f,"
        "\"bms_temp_max_c\":%.1f,\"bms_temp_min_c\":%.1f,\"bms_cycle_count\":%u,"
        "\"bms_charge_mos_on\":%s,\"bms_discharge_mos_on\":%s,\"bms_charger_present\":%s,"
        "\"bms_load_present\":%s,\"bms_balancing\":%s,\"bms_alarm_hex\":\"%s\","
        "\"bms_alarm_summary\":\"%s\",\"bms_cell_voltages_v\":%s}",
        bmsOnline ? "true" : "false", bmsOnline && !bmsFault ? "true" : "false",
        bmsFault ? "true" : "false", bmsState,
        bmsOnline ? static_cast<unsigned long>(millis() - bms.lastRxMs) : 0ul,
        bms.voltage, bms.current, bms.voltage * bms.current / 1000.0f, bms.soc, bms.life,
        bms.remainingAh, bms.cellCount, bms.tempCount, bms.maxCell, bms.minCell,
        bms.maxCellNumber, bms.minCellNumber, (bms.maxCell - bms.minCell) * 1000.0f,
        bms.maxTemp, bms.minTemp, bms.cycleCount,
        bms.chargeMos ? "true" : "false", bms.dischargeMos ? "true" : "false",
        bms.charger ? "true" : "false", bms.load ? "true" : "false",
        bms.balancing ? "true" : "false", alarmHex, bmsFault ? "ALARM" : "NONE", cellsJson);
    if (bmsJsonLength < 0 || static_cast<size_t>(bmsJsonLength) >= sizeof(json) - static_cast<size_t>(jsonLength) + 1u) return;
    jsonLength += bmsJsonLength - 1;  // BMS fields replace the original closing brace.

    if (EV_LOCAL_UDP_ENABLED && WiFi.status() == WL_CONNECTED) {
        // NetworkUDP::write() auto-flushes every 1460 bytes as a NEW datagram,
        // splitting this JSON into independently invalid messages. Native UDP
        // sends one datagram; IP fragmentation/reassembly stays below the app.
        if (telemetrySocket < 0) telemetrySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (telemetrySocket >= 0) {
            sockaddr_in target = {};
            target.sin_family = AF_INET;
            target.sin_port = htons(EV_TELEMETRY_PORT);
            target.sin_addr.s_addr = static_cast<uint32_t>(pitHost);
            const int sent = sendto(telemetrySocket, json, static_cast<size_t>(jsonLength), MSG_DONTWAIT,
                                    reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            if (sent != jsonLength) {
                close(telemetrySocket);
                telemetrySocket = -1;
            }
        }
    }
#if EV_RELAY_ENABLED
    stageRelayTelemetry(json);
#endif
    // Queue USB debug at 1 Hz; never block UART reception for a 3 KB JSON.
    static uint32_t lastUsbMs = 0;
    if (millis() - lastUsbMs >= 1000u && Serial.availableForWrite() >= jsonLength + 2) {
        Serial.println(json);
        lastUsbMs = millis();
    }
    state.configAck = false;
}

}  // namespace

void setup() {
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    Serial.setTimeout(10);
    Serial.println("# DJY ESP gateway boot");
    runStatusLedSelfTest();
    pinMode(EV_PIT_ENABLE_GPIO, INPUT_PULLUP);

    if (EV_REAR_UART_MODE) {
        rearUart.setRxBufferSize(1024); // One extended line exceeds the default 256 bytes.
        rearUart.begin(115200, SERIAL_8N1, EV_REAR_UART_RX_GPIO, EV_REAR_UART_TX_GPIO);
        rearUartReady = static_cast<bool>(rearUart);
        Serial.printf("# Rear UART ready=%u RX GPIO%d TX GPIO%d, 115200 8N1, receive_only=%u\n",
                      rearUartReady, EV_REAR_UART_RX_GPIO, EV_REAR_UART_TX_GPIO, EV_RECEIVE_ONLY);
#if EV_BMS_CAN_ENABLED
        const twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
            static_cast<gpio_num_t>(EV_BMS_CAN_TX_GPIO), static_cast<gpio_num_t>(EV_BMS_CAN_RX_GPIO), TWAI_MODE_NORMAL);
        const twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
        const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        if (twai_driver_install(&general, &timing, &filter) == ESP_OK)
            bmsCanReady = twai_start() == ESP_OK;
        Serial.printf("# DALY CAN ready=%u TX GPIO%d RX GPIO%d 250kbps extended read-only\n",
                      bmsCanReady, EV_BMS_CAN_TX_GPIO, EV_BMS_CAN_RX_GPIO);
#endif
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
    wifiLastReconnectMs = millis();
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
    receiveBmsCan();
    receiveCommands();
    forwardLiveTvCommand();
    sendEspStatus();
    maintainWifi();
    updateStatusLed();
    publishTelemetry();
    delay(1);
}
