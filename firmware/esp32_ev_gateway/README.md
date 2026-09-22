# ESP32 EV Gateway board firmware

This directory is the board-only PlatformIO project for the ESP32-S3 vehicle
gateway. It receives Rear STM32 telemetry over UART, keeps direct USB serial
available for bench diagnostics, and can forward telemetry through local UDP
or the HTTPS relay.

## Files kept in the board build

- `src/main.cpp`: ESP32-S3 firmware
- `include/djy_uart_protocol.h`: Rear STM32 UART framing
- `include/djy_telemetry_protocol.h`: 238-byte Rear telemetry, CRC16, 460800 baud
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

## Rear binary telemetry

The default UART mode now matches **workspace `stm_back/`**, not the older
`firmware/tv_stm_esp/stm_back/` archive. Update Rear and ESP together:
UART1 PA9/PA10 ↔ GPIO18/GPIO17, **460800 8N1**, 238-byte binary telemetry.
The existing nine-byte ESP→Rear command remains unchanged, at the same baud.
Rear USB diagnostics and the IMU retain their own 115200 baud settings.

Rear now requests one snapshot per completed control tick: **100 Hz** on
UART1 (51.65% wire utilization). USB ASCII remains at most 5 Hz. Main-loop
stalls or UART busy conditions remain visible as source sequence gaps.

The HTTPS relay packs the unchanged JSON values into rows, sending column
names once per batch. A 96 KiB FIFO byte buffer holds up to 256 rows; batches
contain up to 64 rows within 64 KiB. HTTPClient streams the frozen batch from
the FIFO without a second full-body buffer. Exchanges start at most five times
per second, with HTTP time included in the 200 ms period. Rows stay queued until
acknowledged; the server de-duplicates retries and commits a whole batch to
SQLite before acknowledging it. The server must be updated before the ESP.

The ESP validates CRC/ranges, recovers framing after corrupt bytes, and maps
the binary fields to the existing JSON names. No page changes are required
for current displays. `rear_sample` adds source sequence/time/drop diagnostics;
the server retains it in the recorded JSON. For intentionally using archived
ASCII Rear firmware, set `EV_REAR_UART_BINARY=0` in local config (115200 baud).
See [wire layout and validation notes](../../docs/BINARY_UART_KO.md).

## BMS power polling and bench measurements

On the separate **250 kbps DALY CAN** connection, the ESP requests voltage,
current and SOC (`0x90`) every **10 ms**. IDs `0x91` through `0x98` have an
independent schedule: one request every 100 ms, or 800 ms per ID. A delayed
loop sends at most one request per schedule and does not replay missed polls.
A failed power enqueue is counted and retried at the next 10 ms opportunity;
failed detail requests retain the same ID and retry after 20 ms.

This is a nominal request schedule, not a guarantee of a fresh BMS measurement
every 10 ms. It does not change the Rear STM's existing 10 ms control loop or
implement measured-power feedback or a 10 kW limiter. The ESP remains a data
collector for BMS voltage/current; those measurements are not yet Rear control
inputs. Existing reverse-direction TV commands are unchanged.

The normal JSON packet includes `bms_power_timing` with these fixed indices:

| Index | Meaning |
| --- | --- |
| 0 | Format version, currently `1` |
| 1 | `0x90` requests accepted into TWAI's transmit queue since boot |
| 2 | `0x90` transmit enqueue failures since boot |
| 3 | Valid `0x90` responses processed since boot, including unchanged values |
| 4 | Age of the last processed `0x90`, in ms; `-1` before any response |
| 5 | Interval between the last two processed `0x90` responses, in ms |
| 6 | Maximum such response interval since boot, in ms |
| 7 | Maximum interval between successful `0x90` request enqueues, in ms |

Intervals are `0` until two relevant events have occurred. Counts are unsigned
32-bit values and wrap modulo 2^32. The intervals and age use ESP `millis()`;
they describe software processing/enqueue times, not CAN wire timestamps or
the BMS ADC sampling time. Frames drained together can have a zero interval.
An accepted transmit does not prove bus acknowledgement or a BMS response.
Other BMS IDs do not refresh the power-specific age; the older `bms_age_ms`
and `bms_online` fields still describe reception of **any** BMS message.

The gateway retains this array in snapshots and recorded JSON. Each valid
`0x90` response creates its own `sample_kind="bms"` event, including identical
readings; each Rear frame creates a `sample_kind="rear"` event. With 100 Hz
BMS replies and Rear frames this is about 200 events/s. Other fields retain
their latest readings and are not newly measured on every event. A 1 Hz
heartbeat is used only when neither source produces events. This does not
prove that no power peak occurred between BMS measurements.

The web dashboard polls every 100 ms, retains every returned sample, plots
battery voltage/current/power and reports backlog/drop diagnostics. History
CSV downloads now cover the entire session. The local `/records` page has
stable history pages plus whole-session JSONL and CSV exports; the raw CSV
includes `payload_json` so all fields survive. Start recording before the run.
The live browser/server buffers hold 12,000 events; older saved events remain
in SQLite. No unlimited on-board storage is provided: a long outage, power
loss, or sustained network throughput below the incoming rate can lose data.
See [rates, limits and verification](../../docs/HIGH_RATE_TELEMETRY_KO.md).

For a bench check, use the packet's ESP `timestamp_ms` and counter deltas to
compare request and response rates. Check enqueue failures, power age, and
maximum intervals, then compare voltage/current step response against an
independent sufficiently fast reference measurement. Use a timestamped CAN
capture to distinguish bus timing from ESP dequeue delays. Do not infer the
internal sampling rate from how often the numeric value changes. Validate
the particular BMS firmware's support for this polling rate before relying
on it for power control.

Host regression checks (from `DJY_EvTLMT-main`):

```sh
python3 -m unittest discover -s tests -p test_esp_bms_packet.py -v
```

The checks exercise the actual firmware's scheduling/decoding/JSON code,
including separate detail polls, TX failure, delayed loops, clock rollover,
power-specific freshness, and maximum-width diagnostics with 48 cell values.
