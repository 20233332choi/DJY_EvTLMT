"""Read-only host audit of current STM text -> ESP/USB -> tuning data.

Requires g++; extracts the real ESP parser and JSON formatter. Sensor inputs,
clock, Wi-Fi and pit permission are simulated. Output describes the current
behavior, including defects; successful execution is NOT a readiness test.
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gateway"))
from ev_gateway import TelemetryStore
from stm_serial import RearStatusParser
from tuning import project


def main():
    stm = (ROOT / "firmware/stm32_rear_uart/Core/Src/main.c").read_text(encoding="utf-8")
    esp = (ROOT / "firmware/esp32_ev_gateway/src/main.cpp").read_text(encoding="utf-8")
    # Use the actual sender's field order and duplicate fields, with synthetic values.
    fmt = stm.split("int n = snprintf(line, sizeof(line),", 1)[1].split("RPM_GetLeft()", 1)[0]
    fmt = "".join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', fmt))
    values = dict(L="1200", R="1300", cap="100/100", glt="0/0", tps="2000",
                  pct="50", idle="868", imu="1", sas="9000", yaw="125", lat="230",
                  lon="410", ax="12", ay="34", az="9810", vs="12000", dy="456",
                  ye="331", dp="910", pl="4200", pr="5100", tva="1", eda="0",
                  tr="875", ctl="1/7", req="50", lim="100", app="50", tv="1",
                  ed="0", fault="0", dac="1200/1300", urx="17", uerr="0",
                  ipk="100", bad="0", rs="0", rv="1/1")
    line = " ".join(f"{token.split('=')[0]}={values[token.split('=')[0]]}" for token in fmt.split())
    legacy = "L=1200 R=1300 cap=100/100 glt=0/0 tps=2000 pct=50 idle=868"
    cases = {"extended": line, "legacy": legacy,
             "extended_no_rv": re.sub(r" rv=\S+", "", line),
             "extended_tps_800": line.replace("tps=2000", "tps=800").replace("pct=50", "pct=0")}
    state = esp[esp.index("struct VehicleState {"):esp.index("} state;") + len("} state;")]
    parser = esp[esp.index("bool parseRearUartLine("):esp.index("void receiveRearUart()")]
    helpers = esp[esp.index("float throttlePercent()"):esp.index("bool pitAllowed()")]
    publish = esp[esp.index("    const bool driverFresh =", esp.index("void publishTelemetry(")):
                  esp.index("    if (jsonLength < 0")]
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include "djy_can_protocol.h"
#define EV_REAR_UART_MODE 1
#define EV_RECEIVE_ONLY 1
#define EV_RELAY_ENABLED 0
#define WL_CONNECTED 3
#define PI 3.14159265358979323846
template<class T> T constrain(T x,T lo,T hi) { return x<lo?lo:x>hi?hi:x; }
uint32_t clockMs=1000;
uint32_t millis() { return clockMs; }
bool liveCommandSeen=false;
struct { uint8_t sequence=0; } liveCommand;
struct { int status() { return 0; } int RSSI() { return -127; } } WiFi;
uint32_t wifiReconnectAttempts=0;
constexpr uint16_t kSasCounts=16384, kSasCenterRaw=8192;
'''
    harness += re.search(r"constexpr size_t kTelemetryJsonSize = .*?;", esp).group() + "\n"
    harness += state + helpers + parser + "\nbool pitAllowed() { return false; }\nvoid emit() {\n"
    harness += publish + '\nif(jsonLength<0 || size_t(jsonLength)>=sizeof(json)) std::exit(2);\nstd::puts(json);\n}\n'
    harness += r'''
int main() {
    std::string line;
    while(std::getline(std::cin,line)) {
        state={}; clockMs=1000;
        if(!parseRearUartLine(line.c_str())) return 3;
        emit(); clockMs=1250; emit(); clockMs=1301; emit();
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="ev-contract-audit-") as temp:
        cpp, exe = pathlib.Path(temp) / "audit.cpp", pathlib.Path(temp) / "audit.exe"
        cpp.write_text(harness, encoding="utf-8")
        subprocess.run(["g++", "-std=c++11", "-I", str(ROOT / "firmware/esp32_ev_gateway/include"),
                        str(cpp), "-o", str(exe)], check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], input="\n".join(cases.values()) + "\n",
                                text=True, capture_output=True, check=True, timeout=10)
    packets = [json.loads(row) for row in result.stdout.splitlines()]
    assert len(packets) == len(cases) * 3
    for index, (name, text) in enumerate(cases.items()):
        usb = RearStatusParser()
        usb.parse(text)
        # Counter advance makes the USB quality check meaningful.
        usb_packet = usb.parse(text.replace("cap=100/100", "cap=110/110"))
        assert usb_packet is not None
        for route, packet in [("ESP", packets[index * 3]), ("STM_USB", usb_packet)]:
            store = TelemetryStore()
            store.update(packet, now=1.0)
            snapshot = store.snapshot(now=1.0)
            row = {"case": name, "route": route,
                   "tps_raw": snapshot["tps_raw"], "tps_pct_received": snapshot["tps_pct"],
                   "tps_online": snapshot["tps_online"], "tps_ok": snapshot["tps_ok"],
                   "sas_raw": snapshot["sas_raw"], "sas_online": snapshot["sas_online"],
                   "plot": project(snapshot)}
            print(json.dumps(row, ensure_ascii=True))
        assert not packets[index * 3 + 2]["stm_online"]
    print("HOST_AUDIT_COMPLETED (synthetic inputs; inspect findings, not a hardware PASS)")


if __name__ == "__main__":
    main()
