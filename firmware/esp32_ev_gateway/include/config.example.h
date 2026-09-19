#pragma once

// Copy this file to config.h. config.h is excluded from Git.
#define EV_WIFI_SSID "YOUR_HOTSPOT_SSID"
#define EV_WIFI_PASSWORD "YOUR_HOTSPOT_PASSWORD"
#define EV_PIT_HOST "192.168.0.10"

#define EV_TELEMETRY_PORT 9003
#define EV_COMMAND_PORT 9006
#define EV_LOCAL_UDP_ENABLED 1
// Bench receive-only gate: rejects USB/UDP/relay control and never sends TV commands.
#define EV_RECEIVE_ONLY 1

// Internet relay through the vehicle phone hotspot and an HTTPS endpoint such
// Direct EV endpoint exposed from pit gateway port 8766 through ngrok.
// Example: https://YOUR-STABLE-DOMAIN/api/vehicle/exchange
#define EV_RELAY_ENABLED 0
#define EV_RELAY_URL "https://YOUR-STABLE-DOMAIN/api/vehicle/exchange"
#define EV_RELAY_TOKEN "REPLACE_WITH_AT_LEAST_16_RANDOM_CHARACTERS"
#define EV_VEHICLE_ID "EV"

// Telemetry-only bench tests may explicitly allow insecure TLS. Remote vehicle
// commands are compile-time blocked whenever insecure TLS is selected.
#define EV_RELAY_ALLOW_INSECURE_TLS 0
#define EV_RELAY_ACCEPT_COMMANDS 0
#define EV_ALLOW_INSECURE_RELAY_COMMANDS 0
// Plain HTTP is an unencrypted, read-only diagnostic fallback. Never enable
// EV_RELAY_ACCEPT_COMMANDS with this option.
#define EV_ALLOW_PLAINTEXT_RELAY 0
// Let's Encrypt Generation Y roots (YE/YR) are included for ngrok's chain.
// EV_RELAY_CA_CERT can override it for another HTTPS endpoint.
// For ngrok use platformio-modern.ini: verified ECDHE-RSA/AES-GCM avoids
// the slow ECDSA certificate handshake on this board. No insecure fallback.

// ESP32-S3 DevKitC-1 built-in RGB status LED. On low-voltage GPIO48 board
// revisions, jumper GPIO38 to GPIO48 and leave this set to 38.
// Blue blink: joining hotspot; yellow blink: relay retrying; green: relay live.
#define EV_STATUS_LED_ENABLED 1
#define EV_STATUS_LED_GPIO 38

#define EV_CAN_TX_GPIO 17
#define EV_CAN_RX_GPIO 18

// DALY R24TS on its separate 250 kbps CAN bus (Rear UART mode only).
#define EV_BMS_CAN_TX_GPIO 9
#define EV_BMS_CAN_RX_GPIO 8

// Active-low physical switch. Leave open for read-only telemetry.
#define EV_PIT_ENABLE_GPIO 4
