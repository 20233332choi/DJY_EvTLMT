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
    def test_bms_can_polling_and_response_ids(self):
        source = (ROOT / "firmware/esp32_ev_gateway/src/main.cpp").read_text(encoding="utf-8")
        state = source[source.index("struct BmsState {"):source.index("DjyUartLiveTv liveCommand")]
        decoder = source[source.index("uint16_t bmsU16("):source.index("bool jsonBoolean(")]
        harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#define EV_BMS_CAN_ENABLED 1
#define ESP_OK 0
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"failed: %s\n",#x); std::exit(1); } } while(0)
struct twai_message_t { uint32_t identifier; bool extd, rtr; uint8_t data_length_code, data[8]; } incoming;
bool pending=false, bmsCanReady=false;
bool txOk=true;
uint32_t clockMs=1000;
uint32_t millis() { return clockMs; }
struct Sent { uint32_t time, id; };
std::vector<Sent> sent;
int twai_receive(twai_message_t* frame,int) { if(!pending)return -1;*frame=incoming;pending=false;return ESP_OK; }
int twai_transmit(twai_message_t* frame,int wait) {
    CHECK(wait==0 && frame->extd && !frame->rtr && frame->data_length_code==8);
    for(auto byte : frame->data) CHECK(byte==0);
    sent.push_back({clockMs,frame->identifier});
    return txOk ? ESP_OK : -1;
}
''' + state + decoder + r'''
void publishTelemetry() { bmsSamplePending=false; }
void reset() {
    bms={}; bmsRequestId=0x91; bmsNextRequestMs=5; bmsNextPowerRequestMs=0;
    sent.clear(); pending=false; txOk=true; bmsCanReady=true; clockMs=0;
}
void receive(uint32_t id) {
    incoming={}; incoming.extd=true; incoming.data_length_code=8; incoming.identifier=id;
    const uint8_t data[8]={1,0x10,0x36,0x10,0x36,0x10,0x36,0};
    std::memcpy(incoming.data,data,8); pending=true; receiveBmsCan();
}
int main() {
    for(uint32_t id=0x90;id<=0x98;++id) {
        bms={}; receive(0x18004001u | (id<<16));
        CHECK(bms.lastRxMs==1000);
        CHECK(bms.powerSeen==(id==0x90));
        if(id==0x91) CHECK(bms.maxCell>0);
        if(id==0x93) CHECK(bms.chargeMos);
        if(id==0x95) CHECK(bms.cellSeen[0] && bms.cells[0]>4.149f && bms.cells[0]<4.151f);
        if(id==0x97) CHECK(bms.balancing);
    }
    for(uint32_t id : {0x18954002u,0x18950140u,0x19954001u,0x18994001u}) {
        bms={};receive(id);CHECK(bms.lastRxMs==0);
    }
    for(int invalid=0;invalid<3;++invalid) {
        bms={}; incoming={}; incoming.identifier=0x18904001u;
        incoming.extd=invalid!=0; incoming.rtr=invalid==1;
        incoming.data_length_code=invalid==2 ? 7 : 8;
        pending=true; receiveBmsCan(); CHECK(!bms.powerSeen && bms.lastRxMs==0);
    }

    // Same-valued frames still count; other IDs must not refresh power data.
    clockMs=0; receive(0x18904001u); CHECK(bms.powerSeen && bms.powerRxCount==1);
    clockMs=70; receive(0x18914001u);
    CHECK(bms.lastRxMs==70 && bms.powerLastRxMs==0 && bms.powerRxCount==1);
    clockMs=80; receive(0x18904001u); CHECK(bms.powerRxIntervalMs==80);
    clockMs=90; receive(0x18904001u);
    CHECK(bms.powerRxCount==3 && bms.powerRxIntervalMs==10 && bms.powerMaxRxIntervalMs==80);

    // Independent schedules: exactly 100 power requests/s, ten detail requests/s.
    reset();
    for(clockMs=0;clockMs<1000;++clockMs) receiveBmsCan();
    uint32_t power=0, detail=0;
    for(auto frame : sent) {
        if(frame.id==0x18900140u) { CHECK(frame.time==power*10); ++power; }
        else {
            CHECK(frame.time==5+detail*100);
            CHECK(frame.id==(0x18000140u|((0x91u+detail%8u)<<16))); ++detail;
        }
    }
    CHECK(power==100 && detail==10 && bms.powerRequestCount==100);
    CHECK(bms.powerMaxRequestIntervalMs==10 && bms.powerRxCount==0);

    // Detail polling must survive a loop running only every 10 ms.
    reset();
    for(clockMs=0;clockMs<1000;clockMs+=10) receiveBmsCan();
    CHECK(bms.powerRequestCount==100 && sent.size()==110);

    // Full TX queue: no busy retry, fabricated reply or skipped detail ID.
    reset(); txOk=false; receiveBmsCan();
    CHECK(bms.powerTxErrors==1 && bms.powerRequestCount==0 && !bms.powerRequested);
    clockMs=1; receiveBmsCan(); CHECK(sent.size()==1);
    clockMs=5; receiveBmsCan(); CHECK(bmsRequestId==0x91);
    txOk=true; clockMs=10; receiveBmsCan(); CHECK(bms.powerRequestCount==1);
    clockMs=25; receiveBmsCan();
    CHECK(bmsRequestId==0x92 && bms.powerTxErrors==1 && bms.powerRxCount==0);

    // Late loops do not enqueue hundreds of old requests to catch up.
    reset(); receiveBmsCan(); clockMs=500; receiveBmsCan();
    CHECK(bms.powerRequestCount==2 && bms.powerMaxRequestIntervalMs==500);
    const auto size=sent.size(); receiveBmsCan(); CHECK(sent.size()==size);

    // Timers and observed intervals remain correct across millis() rollover.
    reset(); clockMs=0xfffffff8u; bmsNextPowerRequestMs=clockMs; bmsNextRequestMs=100;
    receiveBmsCan(); clockMs=0; receiveBmsCan(); CHECK(bms.powerRequestCount==1);
    clockMs=2; receiveBmsCan(); CHECK(bms.powerRequestCount==2 && bms.powerMaxRequestIntervalMs==10);
    bmsCanReady=false;
    clockMs=0xfffffffcu; receive(0x18904001u); clockMs=6; receive(0x18904001u);
    CHECK(bms.powerRxIntervalMs==10 && bms.powerMaxRxIntervalMs==10);
}
'''
        with tempfile.TemporaryDirectory(prefix="djy-bms-can-") as tmp:
            cpp, exe = pathlib.Path(tmp) / "test.cpp", pathlib.Path(tmp) / "test.exe"
            cpp.write_text('#include <initializer_list>\n' + harness, encoding="utf-8")
            result = subprocess.run(compiler(True)+["-std=c++11", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], capture_output=True, timeout=180)
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
#include "telemetry_columns.h"
#define EV_REAR_UART_MODE 1
#define EV_RECEIVE_ONLY 1
#define EV_RELAY_ENABLED 0
#define WL_CONNECTED 3
#define PI 3.14159265358979323846
template<class T> T constrain(T x,T lo,T hi) { return x<lo?lo:x>hi?hi:x; }
uint32_t clockMs=1000000;
bool rearSamplePending=false,bmsSamplePending=false;
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
        harness += r'''
std::cout.write(json,jsonLength); std::cout << "\n";
char columns[3072], row[1536];
if(!telemetryColumns(json,columns,sizeof(columns),row,sizeof(row)))std::exit(2);
std::cout << columns << "\n" << row << "\n";
}
'''
        harness += r'''
int main() {
    for(int count : {0,14,48}) {
        if(!state.timing.parse(" ts=1/4294967295/12/4294967295/4294967295/4294967295/4294967295/4294967295/1/2000/1000/-20000/1/390/4294967295/4294967295/4294967295/4294967295/4294967295"))return 2;
        clockMs+=1000;
        state.rearBinarySeen=true;state.rearSampleSequence=0xffffffffu;
        state.rearSnapshotUs=0xffffffffffffffffull;state.rearTxSkipped=0xffffffffu;
        state.rearMissingSamples=state.rearDuplicateSamples=state.rearClockResets=0xffffffffu;
        state.rearMs=state.rearExtendedMs=clockMs;
        state.tpsMs=state.tqvInternalMs=clockMs;
        state.tpsRaw=count==14 ? 685 : count==48 ? 3020 : 3021;
        state.vehicleSpeedMS=12.0f;
        state.rpmLeft=800; state.rpmRight=1200;
        state.rpmLeftValid=state.rpmRightValid=true;
        state.sasRaw=6897; state.sasCenterRaw=6897; state.steeringRad=-0.125f;
        state.sasValid=count==14;
        bms.lastRxMs=count ? clockMs : 0;
        bms.powerSeen=count!=0;
        bms.powerLastRxMs=clockMs-(count==14 ? 17u : 0xffffffffu);
        bms.powerRequestCount=bms.powerTxErrors=bms.powerRxCount=count ? 0xffffffffu : 0;
        bms.powerRxIntervalMs=bms.powerMaxRxIntervalMs=bms.powerMaxRequestIntervalMs=count ? 0xffffffffu : 0;
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
        self.assertEqual(len(rows), 9, "ESP dropped telemetry while formatting BMS fields")
        schemas = [json.loads(rows[i]) for i in (1,4,7)]
        self.assertEqual(schemas[0], schemas[1])
        self.assertEqual(schemas[1], schemas[2])
        for index, count in enumerate((0, 14, 48)):
            row = rows[index*3]
            packet = json.loads(row)  # Reject a trailing NUL just like the UDP receiver.
            self.assertEqual(dict(zip(schemas[index], json.loads(rows[index*3+2]))), packet)
            self.assertLess(len(rows[index*3+2]), len(row)*0.4)
            self.assertLess(len(row), 4096)
            value = 0xffffffff if count else 0
            age = 17 if count == 14 else 0xffffffff if count else -1
            self.assertEqual(packet['bms_power_timing'], [1, value, value, value, age, value, value, value])
            self.assertEqual(packet['board_timing'][11], -20000)
            self.assertEqual(packet['rear_sample'], [1] + [0xffffffff] * 7)
            self.assertEqual(len(packet['board_timing']), 19)
            self.assertEqual(len(packet["bms_cell_voltages_v"]), count)
            store = TelemetryStore()
            store.update(packet, now=1.0)
            snapshot = store.snapshot(now=1.1)
            self.assertEqual(snapshot['rear_sample'], packet['rear_sample'])
            self.assertEqual(snapshot['bms_power_timing'], packet['bms_power_timing'])
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
