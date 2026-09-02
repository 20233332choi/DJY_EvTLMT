#pragma once

// Copy this file to config.h. config.h is excluded from Git.
#define EV_WIFI_SSID "YOUR_HOTSPOT_SSID"
#define EV_WIFI_PASSWORD "YOUR_HOTSPOT_PASSWORD"
#define EV_PIT_HOST "192.168.0.10"

#define EV_TELEMETRY_PORT 9003
#define EV_COMMAND_PORT 9006
#define EV_LOCAL_UDP_ENABLED 1

// Internet relay through the vehicle phone hotspot and an HTTPS endpoint such
// as https://YOUR-STABLE-DOMAIN/api/vehicle/exchange. Keep the token private.
#define EV_RELAY_ENABLED 0
#define EV_RELAY_URL "https://YOUR-STABLE-DOMAIN/api/vehicle/exchange"
#define EV_RELAY_TOKEN "REPLACE_WITH_AT_LEAST_16_RANDOM_CHARACTERS"
#define EV_VEHICLE_ID "EV"

// Telemetry-only bench tests may explicitly allow insecure TLS. Remote vehicle
// commands are compile-time blocked whenever insecure TLS is selected.
#define EV_RELAY_ALLOW_INSECURE_TLS 0
#define EV_RELAY_ACCEPT_COMMANDS 0
// For verified TLS, define EV_RELAY_CA_CERT as the PEM root CA string used by
// the HTTPS endpoint, then enable command reception only after validation.

#define EV_CAN_TX_GPIO 17
#define EV_CAN_RX_GPIO 18

// Active-low physical switch. Leave open for read-only telemetry.
#define EV_PIT_ENABLE_GPIO 4
