# ESP32 EV Gateway board firmware

This directory is the board-only PlatformIO project for the ESP32-S3 vehicle
gateway. It receives Rear STM32 telemetry over UART, keeps direct USB serial
available for bench diagnostics, and can forward telemetry through local UDP
or the HTTPS relay.

## Files kept in the board build

- `src/main.cpp`: ESP32-S3 firmware
- `include/djy_uart_protocol.h`: Rear STM32 UART framing
- `include/djy_can_protocol.h`: shared vehicle CAN identifiers and payloads
- `include/config.example.h`: local configuration template
- `platformio.ini`: PlatformIO target and build flags

Desktop CMake UI, Python gateway/server, web dashboard, test data, ngrok
scripts, build output, and private Wi-Fi/token configuration are intentionally
outside this board package.

## Configure and build

1. Copy `include/config.example.h` to `include/config.h`.
2. Enter the Wi-Fi hotspot and relay settings in `include/config.h`.
3. Build from this directory with `pio run`.
4. Flash only after checking the connected board and COM port.

`include/config.h` and `.pio/` are ignored so credentials and build artifacts
are not committed. The default example keeps the internet relay and remote
commands disabled. Remote commands must not be enabled with insecure TLS.
