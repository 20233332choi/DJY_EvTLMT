"""Run the imported STM sender through the real ESP and USB parsers, without boards."""
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from host_compiler import compiler

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'firmware/tv_stm_esp/stm_back/Core'
sys.path.insert(0, str(ROOT / 'gateway'))
from stm_serial import RearStatusParser
from ev_gateway import TelemetryStore
from tuning import project


class TvStmEspTests(unittest.TestCase):
    def test_actual_sender_to_esp_and_usb(self):
        stm = (CORE / 'Src/main.c').read_text(encoding='utf-8')
        esp = (ROOT / 'firmware/esp32_ev_gateway/src/main.cpp').read_text(encoding='utf-8')
        sender = stm[stm.index('static void Debug_SendESP(void) {'):stm.index('void HAL_TIM_PeriodElapsedCallback')]
        sas = stm[stm.index('static float SAS_to_SteeringAngle(float raw) {'):]
        sas = sas[:sas.index('\n}') + 2]
        tps = stm[stm.index('static float TPS_to_Fraction(float raw) {'):]
        tps = tps[:tps.index('\n}') + 2]
        state = esp[esp.index('struct VehicleState {'):esp.index('} state;') + len('} state;')]
        parser = esp[esp.index('bool parseRearUartLine('):esp.index('void receiveRearUart()')]
        helpers = esp[esp.index('float throttlePercent()'):esp.index('bool pitAllowed()')]
        harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include "vehicle_params.h"
#include "torque_vectoring.h"
#include "can_comm.h"
#include "can_messages.h"
#include "safety_monitor.h"
#include "djy_can_protocol.h"
#include "rear_uart_line.h"
#include "rear_timing.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while(0)
#define HAL_UART_STATE_READY 0
#define EV_REAR_UART_MODE 1
#define PI 3.14159265358979323846
template<class T> T constrain(T x,T lo,T hi) { return x<lo?lo:x>hi?hi:x; }
constexpr uint16_t kSasCounts=16384, kSasCenterRaw=8192;
bool liveCommandSeen=false;
struct { uint8_t sequence=0; } liveCommand;
uint32_t millis() { return 1000; }
TV_t tv={};
uint16_t s_tps_idle=TPS_PEDAL_IDLE;
SensorData_t input={SAS_CENTER_RAW,2000};
bool front=true, heartbeat=true, stopped=false;
uint8_t hb=0;
SensorData_t CAN_GetSensorData() { return input; }
bool CAN_IsSensorFresh() { return front; }
bool CAN_IsHeartbeatFresh() { return heartbeat; }
uint8_t CAN_GetHeartbeatStatus() { return hb; }
SafeAction_t Safety_GetAction() { return stopped ? SAFE_ACTION_STOP : SAFE_ACTION_NONE; }
FaultCode Safety_GetFaultCode() { return FAULT_NONE; }
unsigned RPM_GetLeft() { return 1200; }
unsigned RPM_GetRight() { return 1300; }
bool RPM_IsLeftFresh() { return true; }
bool RPM_IsRightFresh() { return true; }
bool IMU_IsTelemetryFresh() { return true; }
float IMU_GetYawRate() { return 0.125f; }
float IMU_GetLateralAcc() { return -0.23f; }
float IMU_GetRawAx() { return 0.4f; }
float IMU_GetRawAy() { return -0.25f; }
float IMU_GetRawAz() { return 9.81f; }
uint32_t g_rpm_cap_count_l=100, g_rpm_cap_count_r=100;
uint32_t g_rpm_glitch_l=0, g_rpm_glitch_r=0;
uint32_t g_rpm_desync_l=0, g_rpm_desync_r=0;
volatile uint32_t g_can_rx_count=0xffffffff, g_can_err_count=0, g_can_last_esr=0;
uint32_t g_imu_gyro_ok=0xffffffff, g_imu_pkt_bad=0, g_imu_resync=0, g_imu_dma_restart=0;
uint32_t mask=0;
uint32_t __get_PRIMASK() { return mask; }
void __disable_irq() { mask=1; }
void __set_PRIMASK(uint32_t x) { mask=x; }
struct Uart { int gState=0; const uint8_t* pending=nullptr; std::string sent; } huart1,huart2;
int HAL_UART_Transmit_IT(Uart* u,uint8_t* line,uint16_t n) {
    CHECK(!mask && n<768);
    u->sent.assign(reinterpret_cast<char*>(line),n); u->pending=line; u->gState=1; return 0;
}
''' + sas + tps + sender + state + parser + helpers + r'''
void send() {
    huart1.gState=huart2.gState=0;
    Debug_SendESP();
    CHECK(huart1.sent==huart2.sent && huart1.sent.size()<768);
    RearUartLine line;
    for(char c:huart1.sent) {
        auto result=line.push(c);
        CHECK(result!=RearUartLine::Dropped);
        if(result==RearUartLine::Ready) CHECK(parseRearUartLine(line.data()));
    }
    std::printf("%s",huart1.sent.c_str());
}
int main() {
    tv.vehicle_speed=12.0f; tv.traction_scale=.875f;
    tv.tv_active=true; tv.power_left=4; tv.power_right=5;
    send();
    CHECK(state.sasValid && state.sasCenterRaw==1116 && state.steeringRad==0);
    CHECK(state.rpmLeftValid && state.rpmRightValid && state.imuValid);
    CHECK(state.stmCanRx==0xffffffff && state.stmImuDiag0==0xffffffff);
    CHECK(std::fabs(speedKmh()-43.2f)<.001f && state.driverMs==0);
    CHECK(std::fabs(state.powerRightKw-5)<.001f);
    CHECK(state.pidMs==1000 && state.pidKp==PID_KP && state.pidKi==PID_KI && state.pidKd==PID_KD);
    std::string legacy=huart1.sent.substr(0,huart1.sent.find(" kp="));
    CHECK(parseRearUartLine(legacy.c_str()) && state.pidMs==0);
    CHECK(parseRearUartLine((legacy+" kp=-1 ki=0 kd=0").c_str()) && state.pidMs==0);
    const std::string held=huart1.sent;
    input.tps_raw=2820;
    huart1.gState=0;  // USB still owns the shared buffer.
    Debug_SendESP();
    CHECK(held==reinterpret_cast<const char*>(huart2.pending));
    huart1.gState=1; huart2.gState=0;
    Debug_SendESP();
    CHECK(held==reinterpret_cast<const char*>(huart1.pending));
    input.sas_angle=2116; send();
    CHECK(throttlePercent()==100 && state.steeringRad<-.19f);
    stopped=true; front=false; tv={}; send();
    CHECK(state.fault==FAULT_CAN_TIMEOUT && !state.sasValid);
    CHECK(state.dacLeft==uint16_t(V_THROTTLE_OFF*DAC_CODE_PER_V));
    front=true; heartbeat=false; send(); CHECK(!state.sasValid);
    heartbeat=true; hb=HB_STATUS_SAS_ERR; send(); CHECK(!state.sasValid);
    g_rpm_cap_count_l=g_rpm_cap_count_r=g_rpm_glitch_l=g_rpm_glitch_r=0xffffffff;
    g_rpm_desync_l=g_rpm_desync_r=g_can_err_count=g_can_last_esr=0xffffffff;
    g_imu_pkt_bad=g_imu_resync=g_imu_dma_restart=0xffffffff;
    tv.vehicle_speed=99; tv.desired_yaw=3; tv.yaw_error=-3; tv.delta_power=-40;
    tv.power_left=40; tv.power_right=40; tv.traction_scale=1;
    send(); CHECK(state.pidMs==1000 && state.pidKp==PID_KP);
}
'''
        with tempfile.TemporaryDirectory(prefix='tv-stm-esp-') as temp:
            cpp, exe = Path(temp) / 'test.cpp', Path(temp) / 'test.exe'
            cpp.write_text(harness, encoding='utf-8')
            built = subprocess.run(compiler(True)+['-std=c++11', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(CORE / 'Inc'), '-I', str(ROOT / 'firmware/esp32_ev_gateway/include'),
                                    str(cpp), '-o', str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stderr)
        rows = [line for line in ran.stdout.splitlines() if line]
        self.assertEqual(len(rows), 6)
        parser = RearStatusParser()
        for index, line in enumerate(rows):
            packet = parser.parse(line)
            self.assertIsNotNone(packet)
            store = TelemetryStore()
            store.update(packet, now=1)
            snapshot = store.snapshot(now=1)
            self.assertEqual(snapshot['sas_online'], index < 2)
            self.assertFalse(snapshot['live_control_allowed'])
            self.assertEqual([project(snapshot)[k] for k in ('pid_kp', 'pid_ki', 'pid_kd')], [20, 1, 0])
            self.assertIsNone(project(store.snapshot(now=3))['pid_kp'])
            if index == 1:
                self.assertAlmostEqual(project(snapshot)['sas_deg'], math.degrees(-.191), places=3)
                self.assertEqual(snapshot['sas_center_raw'], 1116)
            self.assertFalse(store.snapshot(now=3)['sas_online'])
        for raw in (684, 685, 3020, 3021):
            packet = parser.parse(rows[0].replace('tps=2000', f'tps={raw}'))
            self.assertEqual(packet['tps_ok'], 685 <= raw <= 3020)
        self.assertFalse(parser.parse(rows[0].replace('sv=1', 'sv=2'))['sas_ok'])
        self.assertIsNone(parser.parse(rows[0].replace('sv=1', 'sv=x')))
        self.assertFalse(parser.parse(rows[0].split(' kp=')[0])['pid_online'])
        self.assertFalse(parser.parse(rows[0].replace('kp=20000', 'kp=-1'))['pid_online'])


if __name__ == '__main__':
    unittest.main()
