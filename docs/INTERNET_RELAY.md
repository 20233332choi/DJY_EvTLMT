# ESP32 vehicle internet relay

## Target layout

```text
Front STM --CAN--> Rear STM --UART--> ESP32
                                      |-- USB serial ----------> pit gateway
                                      `-- phone hotspot/LTE
                                          -- HTTPS/ngrok ------> EV gateway :8766
                                                                    |
                                                                    `-- UDP 9004 --> CMake dashboard
```

The CMake dashboard always consumes the same normalized JSON on UDP 9004. The
gateway chooses a fresh direct USB stream before internet relay data, so plugging
in a cable changes the transport without changing the dashboard contract.

The vehicle starts every internet connection. This works behind a phone hotspot
and carrier NAT without port forwarding. The relay endpoint is:

```text
POST /api/vehicle/exchange
Authorization: Bearer <DJY_EV_RELAY_TOKEN>
X-Vehicle-ID: EV
Content-Type: application/json
```

The request body is the ESP telemetry JSON. The response contains either
`"command": null` or the next `live_tv`/`pit_config` command. Live TQV commands
still expire in the Rear STM after 500 ms and return toward 50:50.

## Pit server

Generate one random token and keep it outside Git, chat, screenshots, and logs.
Set it in the pit PC process environment, then start the gateway. Add
`--enable-control` only when vehicle commands are intentionally required.

```powershell
$env:DJY_EV_RELAY_TOKEN = '<private random token>'
python .\gateway\ev_gateway.py
.\run_dashboard.bat ev
```

After the private ESP `config.h` contains the matching URL and token, the
one-click launchers are:

```powershell
.\run_internet_pit_dashboard.bat  # read-only telemetry
.\run_internet_pit_control.bat    # bidirectional TQV control
```

The launcher stops duplicate EV gateway processes, binds the EV server to
loopback port 8766, replaces an existing non-EV ngrok tunnel, checks that the
active public URL matches the ESP build, and then opens the CMake dashboard.
The Baja server does not run in this path.

The pit display is the native CMake dashboard, which receives telemetry on UDP
`9004` and sends commands only to `127.0.0.1:9005`. Requests forwarded by
ngrok cannot call `/api/control` or `/api/test`; it exposes the authenticated
vehicle exchange directly from the EV gateway.

The CMake control panel includes `TEST LINK (NO STM OUTPUT)`. It sends a local
`relay_ping` through UDP 9005, the gateway response, ngrok, and the ESP. The
gateway marks it `ACKED` only after the ESP command counter returns in a later
telemetry exchange. This test never writes an STM CAN/UART control value.

## ngrok

Expose the EV gateway port directly:

```powershell
ngrok http 8766
```

The public EV exchange endpoint is:

```text
https://<stable-ngrok-domain>/api/vehicle/exchange
```

The Baja server does not need to run for the EV pit dashboard.

## ESP private configuration

Copy `firmware/esp32_ev_gateway/include/config.example.h` to the ignored
`config.h`, then configure the vehicle hotspot and relay values:

```c
#define EV_RELAY_ENABLED 1
#define EV_RELAY_URL "https://<stable-ngrok-domain>/api/vehicle/exchange"
#define EV_RELAY_TOKEN "<same private token>"
#define EV_VEHICLE_ID "EV"
```

For an uplink-only bench test, commands must remain disabled:

```c
#define EV_RELAY_ALLOW_INSECURE_TLS 1
#define EV_RELAY_ACCEPT_COMMANDS 0
```

For bidirectional vehicle use, install the endpoint root CA as
`EV_RELAY_CA_CERT`, disable insecure TLS, and then enable commands:

```c
#define EV_RELAY_ALLOW_INSECURE_TLS 0
#define EV_RELAY_ACCEPT_COMMANDS 1
```

The firmware intentionally refuses to compile if remote commands and insecure
TLS are enabled together. A stable ngrok domain is required because changing the
public URL requires rebuilding the ESP configuration.

## Link meanings

- `USB`: ESP USB serial is the active telemetry path.
- `WIFI_UDP`: ESP and pit PC are on the same local network.
- `INTERNET_RELAY`: ESP is reaching the gateway through HTTPS/ngrok.
- `relay_link_online`: a valid authenticated exchange arrived within 1.5 s.
- `internet_relay_errors`: ESP HTTPS attempts that failed.

Host builds do not prove hotspot coverage, TLS trust, ngrok availability, or
vehicle wiring. Validate those separately with drive outputs disabled.
