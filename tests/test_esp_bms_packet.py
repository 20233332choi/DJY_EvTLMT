"""Compile the real ESP formatter and parse the exact UDP payload on the host."""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from host_compiler import compiler

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "gateway"))
from ev_gateway import TelemetryStore


class EspBmsPacketTests(unittest.TestCase):
    def test_bms_can_response_ids(self):
        source = (ROOT / "firmware/esp32_ev_gateway/src/main.cpp").read_text(encoding="utf-8")
        state = source[source.index("struct BmsState {"):source.index("} bms;") + len("} bms;")]
        decoder = source[source.index("uint16_t bmsU16("):source.index("bool jsonBoolean(")]
        harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#define EV_BMS_CAN_ENABLED 1
#define ESP_OK 0
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"failed: %s\n",#x); std::exit(1); } } while(0)
struct twai_message_t { uint32_t identifier; bool extd, rtr; uint8_t data_length_code, data[8]; } incoming;
bool pending=false, bmsCanReady=false;
uint8_t bmsRequestId=0x90;
uint32_t bmsNextRequestMs=0;
uint32_t millis() { return 1000; }
int twai_receive(twai_message_t* frame,int) { if(!pending)return -1;*frame=incoming;pending=false;return ESP_OK; }
int twai_transmit(twai_message_t*,int) { return -1; }
''' + state + decoder + r'''
void receive(uint32_t id) {
    incoming={}; incoming.extd=true; incoming.data_length_code=8; incoming.identifier=id;
    const uint8_t data[8]={1,0x10,0x36,0x10,0x36,0x10,0x36,0};
    std::memcpy(incoming.data,data,8); pending=true; receiveBmsCan();
}
int main() {
    for(uint32_t id=0x90;id<=0x98;++id) {
        bms={}; receive(0x18004001u | (id<<16));
        CHECK(bms.lastRxMs==1000);
        if(id==0x91) CHECK(bms.maxCell>0);
        if(id==0x93) CHECK(bms.chargeMos);
        if(id==0x95) CHECK(bms.cellSeen[0] && bms.cells[0]>4.149f && bms.cells[0]<4.151f);
        if(id==0x97) CHECK(bms.balancing);
    }
    for(uint32_t id : {0x18954002u,0x18950140u,0x19954001u,0x18994001u}) {
        bms={};receive(id);CHECK(bms.lastRxMs==0);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="djy-bms-can-") as tmp:
            cpp, exe = pathlib.Path(tmp) / "test.cpp", pathlib.Path(tmp) / "test.exe"
            cpp.write_text('#include <initializer_list>\n' + harness, encoding="utf-8")
            result = subprocess.run(compiler(True)+["-std=c++11", str(cpp), "-o", str(exe)], capture_output=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            result = subprocess.run([str(exe)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_complete_bms_datagram(self):
        source = (ROOT / "firmware/esp32_ev_gateway/src/main.cpp").read_text(encoding="utf-8")
        state = source[source.index("struct VehicleState {"):source.index("} bms;") + len("} bms;")]
        helpers = source[source.index("float throttlePercent()"):source.index("bool pitAllowed()")]
        publish = source[source.index("void publishTelemetry() {"):source.index("    if (EV_LOCAL_UDP_ENABLED &&", source.index("void publishTelemetry() {"))]
        harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "djy_can_protocol.h"
#include "rear_timing.h"
#define EV_REAR_UART_MODE 1
#define EV_RECEIVE_ONLY 1
#define EV_RELAY_ENABLED 0
#define WL_CONNECTED 3
#define PI 3.14159265358979323846
template<class T> T constrain(T x,T lo,T hi) { return x<lo?lo:x>hi?hi:x; }
uint32_t clockMs=1000000;
bool rearSamplePending=false;
uint32_t millis() { return clockMs; }
uint32_t wifiReconnectAttempts=0;
constexpr uint16_t kSasCounts=16384, kSasCenterRaw=8192;
struct { int status() { return 3; } int RSSI() { return -60; } } WiFi;
bool pitAllowed() { return false; }
size_t strlcat(char* dst,const char* src,size_t size) {
    size_t n=std::strlen(dst), m=std::strlen(src);
    if(n<size) std::snprintf(dst+n,size-n,"%s",src);
    return n+m;
}
'''
        harness += re.search(r"constexpr size_t kTelemetryJsonSize = .*?;", source).group() + "\n"
        harness += state + helpers + publish
        harness += '\nstd::cout.write(json,jsonLength); std::cout << "\\n";\n}\n'
        harness += r'''
int main() {
    for(int count : {0,14,48}) {
        if(!state.timing.parse(" ts=1/4294967295/12/4294967295/4294967295/4294967295/4294967295/4294967295/1/2000/1000/-20000/1/390/4294967295/4294967295/4294967295/4294967295/4294967295"))return 2;
        clockMs+=1000;
        state.rearMs=state.rearExtendedMs=clockMs;
        state.tpsMs=state.tqvInternalMs=clockMs;
        state.tpsRaw=count==14 ? 685 : count==48 ? 3020 : 3021;
        state.vehicleSpeedMS=12.0f;
        state.rpmLeft=800; state.rpmRight=1200;
        state.rpmLeftValid=state.rpmRightValid=true;
        state.sasRaw=6897; state.sasCenterRaw=6897; state.steeringRad=-0.125f;
        state.sasValid=count==14;
        bms.lastRxMs=count ? clockMs : 0;
        bms.voltage=58.1f; bms.soc=100.0f; bms.cellCount=count;
        for(int i=0;i<count;++i) { bms.cells[i]=4.15f; bms.cellSeen[i]=true; }
        publishTelemetry();
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="djy-bms-packet-") as tmp:
            cpp, exe = pathlib.Path(tmp) / "test.cpp", pathlib.Path(tmp) / "test.exe"
            cpp.write_text(harness, encoding="utf-8")
            result = subprocess.run(compiler(True)+["-std=c++11", "-I", str(ROOT / "firmware/esp32_ev_gateway/include"), str(cpp), "-o", str(exe)], capture_output=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            result = subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)
        rows = result.stdout.splitlines()
        self.assertEqual(len(rows), 3, "ESP dropped telemetry while formatting BMS fields")
        for count, row in zip((0, 14, 48), rows):
            packet = json.loads(row)  # Reject a trailing NUL just like the UDP receiver.
            self.assertEqual(packet['board_timing'][11], -20000)
            self.assertEqual(len(packet['board_timing']), 19)
            self.assertEqual(len(packet["bms_cell_voltages_v"]), count)
            store = TelemetryStore()
            store.update(packet, now=1.0)
            snapshot = store.snapshot(now=1.1)
            self.assertEqual(snapshot["bms_online"], bool(count))
            self.assertEqual(snapshot["battery_pack_voltage_v"], 58.1)
            self.assertEqual(snapshot["sas_online"], count == 14)
            self.assertEqual(snapshot["sas_center_raw"], 6897)
            self.assertEqual(snapshot["tps_ok"], bool(count))
            self.assertAlmostEqual(snapshot["speed_kmh"], 21.2057504117, places=2)
            self.assertEqual(packet["limit_reason"], "NONE")
            self.assertAlmostEqual(snapshot["sas_deg"], -7.162, places=2)
            self.assertFalse(store.snapshot(now=5.0)["sas_online"])
            self.assertFalse(store.snapshot(now=5.0)["bms_online"])


if __name__ == "__main__":
    unittest.main()
