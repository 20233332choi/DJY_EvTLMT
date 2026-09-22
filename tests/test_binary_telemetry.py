"""Exercise the workspace Rear sender, ESP receiver, and wire recovery without boards."""
import binascii
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from host_compiler import compiler

ROOT = Path(__file__).resolve().parents[1]
REAR = ROOT / "firmware/stm32_rear_binary/Core"
ESP = ROOT / "firmware/esp32_ev_gateway"


class BinaryTelemetryTests(unittest.TestCase):
    def run_host(self, source, cpp=False):
        with tempfile.TemporaryDirectory(prefix="ev-binary-test-") as temp:
            path = Path(temp) / ("test.cpp" if cpp else "test.c")
            exe = Path(temp) / "test"
            path.write_text(source, encoding="utf-8")
            build = subprocess.run(
                compiler(cpp) + ["-std=c++11" if cpp else "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-g",
                    "-I", str(REAR / "Inc"), "-I", str(ESP / "include"),
                    str(path), "-o", str(exe)], capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr.decode())
            result = subprocess.run([str(exe)], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            return result.stdout

    def test_wire_and_corruption_recovery(self):
        self.assertEqual((REAR / "Inc/djy_telemetry_protocol.h").read_bytes(),
                         (ESP / "include/djy_telemetry_protocol.h").read_bytes())
        wire = self.run_host(r'''
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include "djy_telemetry_protocol.h"
static unsigned feed(DjyTelemetryRx *rx, const uint8_t *data, unsigned n) {
    unsigned count=0;
    for(unsigned i=0;i<n;i++) {
        if(djy_telemetry_push(rx,data[i])) {
            DjyTelemetry out;
            assert(djy_telemetry_unpack(&out,rx->bytes,DJY_TELEMETRY_SIZE));
            assert(out.sequence==0x12345678u);
            ++count;
        }
        assert(rx->used<DJY_TELEMETRY_SIZE);
    }
    return count;
}
int main(void) {
    assert(djy_tm_crc16((const uint8_t *)"123456789",9)==0x29b1u);
    DjyTelemetry p={0}, decoded={0};
    p.sequence=0x12345678u;p.snapshot_us=0x0102030405060708ull;
    p.rpm_left=3000;p.rpm_right=65535;p.tps_raw=4095;p.tps_idle=885;
    p.sas_raw=16383;p.sas_center=1116;p.dac_left=4095;p.traction_milli=1000;
    p.flags=DJY_TM_FLAGS_MASK;p.tps_pct=100;p.requested=p.limit=p.applied=100;
    p.yaw_milli=INT32_MIN;p.lat_milli=INT32_MAX;p.delta_power_milli=-1234;
    p.steer_milli=-3142;p.capture_left=UINT32_MAX;
    p.timing_valid=1;p.timing[0]=1;p.timing[2]=12;
    p.timing[11]=(uint32_t)-1500;p.timing[13]=65535;p.timing[18]=UINT32_MAX;
    uint8_t good[DJY_TELEMETRY_SIZE],bad[DJY_TELEMETRY_SIZE+1];
    djy_telemetry_pack(good,&p);
    assert(djy_telemetry_unpack(&decoded,good,sizeof(good)));
    assert(decoded.snapshot_us==p.snapshot_us && decoded.yaw_milli==INT32_MIN);
    assert(decoded.lat_milli==INT32_MAX && decoded.rpm_right==65535);
    assert(djy_tm_signed(decoded.timing[11])==-1500);
    assert(!djy_telemetry_unpack(&decoded,good,sizeof(good)-1));
    // Every single-bit corruption must be rejected; the next good frame survives.
    for(unsigned i=0;i<sizeof(good);i++)for(unsigned b=0;b<8;b++) {
        memcpy(bad,good,sizeof(good));bad[i]^=(uint8_t)(1u<<b);
        DjyTelemetryRx rx={0};
        assert(feed(&rx,bad,sizeof(good))==0);
        assert(feed(&rx,good,sizeof(good))==1);
    }
    // Missing/inserted bytes, fragmented headers, and truncated frame prefixes.
    for(unsigned i=0;i<sizeof(good);i++) {
        DjyTelemetryRx rx={0};
        memcpy(bad,good,i);memcpy(bad+i,good+i+1,sizeof(good)-i-1);
        assert(feed(&rx,bad,sizeof(good)-1)==0);
        assert(feed(&rx,good,sizeof(good))==1);
        rx=(DjyTelemetryRx){0};
        memcpy(bad,good,i);bad[i]=0x7e;memcpy(bad+i+1,good+i,sizeof(good)-i);
        assert(feed(&rx,bad,sizeof(good)+1)==(i==0?1u:0u));
        assert(feed(&rx,good,sizeof(good))==1);
        rx=(DjyTelemetryRx){0};
        assert(feed(&rx,good,i)==0);
        assert(feed(&rx,good,sizeof(good))==1);
    }
    DjyTelemetryRx rx={0};
    const uint8_t noise[]={0,0xff,0xd5,0xd5,0x4a,99,2,230,0};
    assert(feed(&rx,noise,sizeof(noise))==0);
    assert(feed(&rx,good,sizeof(good))==1);
    assert(feed(&rx,good,sizeof(good))==1);
    assert(rx.errors>0);
    // Valid CRC is not enough: invalid field ranges cannot modify the destination.
    p.requested=101;djy_telemetry_pack(bad,&p);decoded.sequence=99;
    assert(!djy_telemetry_unpack(&decoded,bad,sizeof(good)) && decoded.sequence==99);
    p.requested=100;p.flags=128;djy_telemetry_pack(bad,&p);
    assert(!djy_telemetry_unpack(&decoded,bad,sizeof(good)));
    fwrite(good,1,sizeof(good),stdout);
}
''')
        self.assertEqual(len(wire), 238)
        self.assertEqual(wire[:6], bytes.fromhex("D5 4A 01 02 E6 00"))
        self.assertEqual(struct.unpack_from("<IQI", wire, 6),
                         (0x12345678, 0x0102030405060708, 0))
        self.assertEqual(wire[22:26], bytes.fromhex("B8 0B FF FF"))
        self.assertEqual(struct.unpack_from("<ii", wire, 48), (-2147483648, 2147483647))
        self.assertEqual(struct.unpack_from("<i", wire, 160 + 11 * 4)[0], -1500)
        self.assertEqual(int.from_bytes(wire[-2:], "little"), binascii.crc_hqx(wire[:-2], 0xffff))

    def test_actual_sender_and_esp_receiver(self):
        stm = (REAR / "Src/main.c").read_text()
        esp = (ESP / "src/main.cpp").read_text()
        def function(text, start):
            body = text[text.index(start):]
            return body[:body.index("\n}") + 2]
        sender = function(stm, "static void Telemetry_Publish(uint32_t sequence) {")
        sas = function(stm, "static float SAS_to_SteeringAngle(float raw) {")
        tps = function(stm, "static float TPS_to_Fraction(float raw) {")
        state = esp[esp.index("struct VehicleState {"):esp.index("} state;") + len("} state;")]
        decoder = esp[esp.index("bool parseRearTelemetry("):esp.index("bool parseRearUartLine(")]
        receive = function(esp, "void receiveRearUart() {")
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include "vehicle_params.h"
#include "torque_vectoring.h"
#include "can_comm.h"
#include "can_messages.h"
#include "safety_monitor.h"
#include "djy_can_protocol.h"
#include "djy_uart_protocol.h"
#include "djy_telemetry_protocol.h"
#include "rear_timing.h"
#define HAL_UART_STATE_READY 0
#define HAL_OK 0
#define EV_REAR_UART_BINARY 1
#define EV_REAR_UART_RX_GPIO 18
#define HIGH 1
#define PI 3.14159265358979323846
template<class T> T constrain(T x,T lo,T hi) { return x<lo?lo:x>hi?hi:x; }
uint32_t clockMs=1000,mask=0;
uint64_t sampleUs=0x123456789ull;
uint32_t millis() { return clockMs; }
uint64_t VehicleClock_NowUs() { return sampleUs; }
uint32_t __get_PRIMASK() { return mask; }
void __disable_irq() { mask=1; }
void __set_PRIMASK(uint32_t x) { mask=x; }
TV_t tv={};
uint16_t s_tps_idle=TPS_PEDAL_IDLE;
SensorData_t input={SAS_CENTER_RAW,2000};
bool front=true,imu=true;
SensorData_t CAN_GetSensorData() { return input; }
bool CAN_IsSensorFresh() { return front; }
bool CAN_IsHeartbeatFresh() { return front; }
uint8_t CAN_GetHeartbeatStatus() { return 0; }
FaultCode Safety_GetFaultCode() { return front?FAULT_NONE:FAULT_CAN_TIMEOUT; }
unsigned RPM_GetLeft() { return 3000; }
unsigned RPM_GetRight() { return 3100; }
bool RPM_IsLeftFresh() { return true; }
bool RPM_IsRightFresh() { return false; }
bool IMU_IsTelemetryFresh() { return imu; }
float IMU_GetYawRate() { return -.125f; }
float IMU_GetLateralAcc() { return -.25f; }
float IMU_GetAccelerationX() { return 1.25f; }
float IMU_GetAccelerationY() { return -2.5f; }
float IMU_GetAccelerationZ() { return 9.81f; }
float TV_GetStrength() { return .5f; }
bool EspLink_GetLiveTv(DjyUartLiveTv *p) { p->sequence=7;p->strength_percent=55;p->limit_percent=80;return true; }
bool Timing_ReadRelay(uint32_t *p) { assert(!mask);memset(p,0,76);p[0]=1;p[2]=12;p[11]=(uint32_t)-1500;return true; }
uint32_t g_rpm_cap_count_l=200,g_rpm_cap_count_r=201,g_rpm_glitch_l=0,g_rpm_glitch_r=1;
uint32_t g_esp_rx_count=99,g_esp_crc_err=1,g_esp_uart_err=2;
volatile uint32_t g_can_rx_count=UINT32_MAX,g_can_err_count=2,g_can_last_esr=3;
uint32_t g_imu_gyro_ok=300,g_imu_pkt_bad=1,g_imu_resync=2,g_imu_dma_restart=3;
struct { uint32_t DOR1=1200,DOR2=1300; } dacRegs;
#define DAC (&dacRegs)
struct Uart { int gState=0;const uint8_t *pending=nullptr;std::string sent; } huart1,huart2;
bool failTx=false;
int HAL_UART_Transmit_IT(Uart *u,uint8_t *data,uint16_t n) {
    assert(!mask);
    if(u==&huart1 && failTx)return 1;
    u->sent.assign((char*)data,n);u->pending=data;u->gState=1;return HAL_OK;
}
bool liveCommandSeen=true;
DjyUartLiveTv liveCommand={7,55,80,1};
bool rearSamplePending=false;
struct { std::deque<uint8_t> bytes;int available(){return bytes.size();}
    int read(){int v=bytes.front();bytes.pop_front();return v;} } rearUart;
int digitalRead(int) { return HIGH; }
void publishTelemetry();
''' + sas + tps + sender + state + decoder + receive + r'''
std::vector<uint32_t> published;
void publishTelemetry() { assert(rearSamplePending);rearSamplePending=false;published.push_back(state.rearSampleSequence); }
void feed(const std::string& s) { for(unsigned char b:s)rearUart.bytes.push_back(b);receiveRearUart(); }
int main() {
    tv.vehicle_speed=12;tv.desired_yaw=.25f;tv.yaw_error=.375f;tv.delta_power=-.5f;
    tv.power_left=4.5f;tv.power_right=4;tv.traction_scale=.875f;tv.tv_active=true;
    Telemetry_Publish(1);
    const std::string first=huart1.sent,usb=huart2.sent;
    assert(first.size()==238 && usb.find("L=3000 R=3100")==0);
    assert(djy_tm_frame_valid((const uint8_t*)first.data(),first.size()));
    feed(first.substr(0,3));assert(published.empty());
    feed(first.substr(3));assert(published.size()==1);
    assert(state.rpmLeft==3000 && state.rpmRight==3100);
    assert(state.rpmLeftValid && !state.rpmRightValid && state.imuValid);
    assert(state.steeringRad==0 && state.sasValid && state.sasCenterRaw==SAS_CENTER_RAW);
    assert(state.yawRateRadS==-.125f && state.deltaPowerKw==-.5f);
    assert(state.imuRawAx==1.25f && state.longitudinalAccelMS2==1.25f);
    assert(state.pidKp==PID_KP && state.pidKi==PID_KI && state.pidKd==PID_KD);
    assert(state.stmCanRx==UINT32_MAX && state.rearCommandRx==99 && state.rearCommandErrors==3);
    assert(state.tvRequested==55 && state.tvApplied==50 && state.tvLimit==80 && state.configAck);
    assert(state.timing.valid && state.timing.fields[11]==-1500);
    assert(state.rearSnapshotUs==sampleUs && state.rearTxSkipped==0);
    feed(first);assert(published.size()==1 && state.rearDuplicateSamples==1);
    Telemetry_Publish(2); // UART1 busy: buffers remain untouched.
    assert(first==std::string((const char*)huart1.pending,first.size()));
    assert(usb==std::string((const char*)huart2.pending,usb.size()));
    huart1.gState=0;sampleUs+=200000;Telemetry_Publish(3); // USB still busy.
    assert(usb==std::string((const char*)huart2.pending,usb.size()));
    feed(huart1.sent);assert(state.rearMissingSamples==1 && state.rearTxSkipped==1);
    huart1.gState=0;failTx=true;Telemetry_Publish(4);
    failTx=false;sampleUs+=200000;Telemetry_Publish(5);
    feed(huart1.sent);assert(state.rearMissingSamples==2 && state.rearTxSkipped==2);
    // Corrupt input cannot overwrite the last accepted sample.
    std::string broken=huart1.sent;broken[50]^=1;
    feed(broken);assert(state.rearSampleSequence==5);
    huart1.gState=0;sampleUs+=200000;front=imu=false;Telemetry_Publish(6);
    const std::string sixth=huart1.sent;
    huart1.gState=0;sampleUs+=200000;Telemetry_Publish(7);
    feed(sixth+huart1.sent);assert(published[published.size()-2]==6 && published.back()==7);
    assert(!state.sasValid && !state.imuValid && state.fault==FAULT_CAN_TIMEOUT);
    // Timeout discards an unfinished packet; next complete packet resynchronizes.
    feed(sixth.substr(0,10));clockMs+=60;receiveRearUart();
    huart1.gState=0;sampleUs+=200000;Telemetry_Publish(8);
    feed(huart1.sent);assert(published.back()==8);
    assert(state.rearUartErrors>=3);
    // Simulate a fresh STM boot, and then sequence wrap without a clock rollback.
    DjyTelemetry p={};assert(djy_telemetry_unpack(&p,(const uint8_t*)huart1.sent.data(),238));
    uint8_t wire[DJY_TELEMETRY_SIZE];p.sequence=1;p.snapshot_us=1000;djy_telemetry_pack(wire,&p);
    feed(std::string((char*)wire,238));assert(state.rearClockResets==1);
    state.rearSampleSequence=UINT32_MAX;p.sequence=0;p.snapshot_us=2000;djy_telemetry_pack(wire,&p);
    uint32_t gaps=state.rearMissingSamples;feed(std::string((char*)wire,238));
    assert(state.rearMissingSamples==gaps && state.rearSampleSequence==0);
    // Command direction is still the existing nine-byte protocol.
    uint8_t command[DJY_UART_LIVE_TV_SIZE];djy_uart_pack_live_tv(command,&liveCommand);
    DjyUartLiveTv c={};assert(sizeof(command)==9 && djy_uart_unpack_live_tv(&c,command));
    assert(c.strength_percent==55);
}
'''
        self.run_host(harness, cpp=True)


if __name__ == "__main__":
    unittest.main()
