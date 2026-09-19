"""Compile the actual ESP line assembler and parser on the host (no hardware writes)."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from host_compiler import compiler

ROOT = pathlib.Path(__file__).resolve().parents[1]


class RearUartTests(unittest.TestCase):
    def test_real_parser_and_line_assembly(self):
        source = (ROOT / "firmware/esp32_ev_gateway/src/main.cpp").read_text(encoding="utf-8")
        state = source[source.index("struct VehicleState {"):source.index("} state;") + len("} state;")]
        parser = source[source.index("bool parseRearUartLine("):source.index("void receiveRearUart()")]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "rear_uart_line.h"
#include "rear_timing.h"
#include "djy_can_protocol.h"
#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
template<class T> T constrain(T x, T lo, T hi) { return x < lo ? lo : x > hi ? hi : x; }
uint32_t millis() { return 1000u; }
bool liveCommandSeen = false;
struct { uint8_t sequence = 0; } liveCommand;
''' + state + parser + r'''
int main() {
    const std::string legacy = "L=123 R=456 cap=4294967295/4294967295 glt=0/0 tps=868 pct=0 idle=868";
    const std::string extended = legacy +
      " imu=1 sas=8192 yaw=125 lat=-230 lon=410 ax=12 ay=-34 az=9810"
      " vs=12345 dy=456 ye=-78 dp=910 pl=11200 pr=10300 tva=1 eda=0 tr=875"
      " ctl=1/7 req=50 lim=60 app=40 tv=1 ed=0 fault=0 dac=1200/1300 sas=8192 imu=1 urx=17 uerr=3";
    assert(extended.size() > 160);
    assert(parseRearUartLine(extended.c_str()));
    assert(state.rpmLeft == 123 && state.rpmRight == 456);
    assert(state.rearUartRx == 1 && state.rearUartErrors == 0);
    assert(state.rearCommandRx == 17 && state.rearCommandErrors == 3);
    assert(state.rearExtendedMs == 1000 && state.sasMs == 1000);
    assert(state.tvApplied == 40 && state.imuValid);
    assert(state.tqvInternalMs == 1000);
    assert(state.vehicleSpeedMS > 12.34f && state.vehicleSpeedMS < 12.35f);
    assert(state.desiredYawRadS > 0.455f && state.desiredYawRadS < 0.457f);
    assert(state.yawErrorRadS < -0.077f && state.yawErrorRadS > -0.079f);
    assert(state.deltaPowerKw > 0.909f && state.deltaPowerKw < 0.911f);
    assert(state.powerLeftKw > 11.19f && state.powerLeftKw < 11.21f);
    assert(state.powerRightKw > 10.29f && state.powerRightKw < 10.31f);
    assert(state.tractionScale > 0.874f && state.tractionScale < 0.876f);
    assert(!parseRearUartLine("L=1 R=2"));
    assert(!parseRearUartLine("L=1 R=2 cap=0/0 glt=0/0 tps=9999 pct=0 idle=868"));
    assert(!parseRearUartLine("L=1 R=2 cap=0/0 glt=0/0 tps=868 pct=-1 idle=868"));
    assert(state.rearUartRx == 1);
    assert(parseRearUartLine(legacy.c_str()));
    assert(state.rearUartRx == 2 && state.rearExtendedMs == 0);
    assert(!state.rpmLeftValid && !state.rpmRightValid);
    assert(parseRearUartLine((extended + " rv=0/1").c_str()));
    assert(!state.rpmLeftValid && state.rpmRightValid);
    assert(parseRearUartLine((extended + " rv=1/0").c_str()));
    assert(state.rpmLeftValid && !state.rpmRightValid);
    assert(!parseRearUartLine((extended + " rv=2/0").c_str()));
    assert(parseRearUartLine(legacy.c_str()));
    assert(!state.rpmLeftValid && !state.rpmRightValid);
    assert(state.sasMs == 0 && state.driverMs == 0 && !state.imuValid);
    const std::string team = "L=0 R=0 cap=0/0 glt=0/0 dsy=3/4 tps=896 pct=0 idle=897 can=14369/2/0000000a imu=12859/1/282887/3";
    assert(parseRearUartLine(team.c_str()));
    assert(state.tpsRaw == 896 && state.tpsReportedPct == 0);
    assert(state.desyncLeft == 3 && state.desyncRight == 4);
    assert(state.stmCanRx == 14369 && state.stmCanErrors == 2 && state.stmCanStatus == 10);
    assert(state.stmImuDiag0 == 12859 && state.stmImuDiag1 == 1);
    assert(state.stmImuDiag2 == 282887 && state.stmImuDiag3 == 3);
    assert(!state.imuValid && state.tqvInternalMs == 0);
    RearUartLine line;
    for (char c : extended + "\r") assert(line.push(c) == RearUartLine::Pending);
    assert(line.push('\n') == RearUartLine::Ready);
    assert(extended == line.data());
    for (int i=0; i<768; ++i) assert(line.push('x') == RearUartLine::Pending);
    for (char c : legacy) assert(line.push(c) == RearUartLine::Pending);
    assert(line.push('\n') == RearUartLine::Dropped);
    for (char c : legacy) line.push(c);
    assert(line.push('\n') == RearUartLine::Ready);
    assert(legacy == line.data());
    const std::string timing=" ts=1/200/10/100/50/90/100/130/1/600/400/-1500/1/390/5/0/0/0/2000000";
    assert(parseRearUartLine((extended+timing).c_str()));
    assert(state.timing.valid && state.timing.fields[11]==-1500);
    char timing_json[256];assert(state.timing.json(timing_json,sizeof(timing_json)));
    assert(strstr(timing_json,"-1500")!=nullptr);
    assert(parseRearUartLine(legacy.c_str()) && !state.timing.valid);
    assert(!state.timing.parse(" ts=1/2/3"));
    assert(!state.timing.parse((timing+"/999").c_str()));
    assert(!state.timing.parse(" ts=1/-2/10/100/50/90/100/130/1/600/400/-1500/1/390/5/0/0/0/2000000"));
    line.push('L'); line.push('\0'); line.push('x');
    assert(line.push('\n') == RearUartLine::Dropped);
    assert(line.push('\n') == RearUartLine::Ready && line.data()[0] == '\0');
}
'''
        with tempfile.TemporaryDirectory(prefix="djy-uart-test-") as tmp:
            cpp = pathlib.Path(tmp) / "test.cpp"
            executable = pathlib.Path(tmp) / "test.exe"
            cpp.write_text(harness, encoding="utf-8")
            subprocess.run(compiler(True)+["-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "firmware/esp32_ev_gateway/include"),
                            str(cpp), "-o", str(executable)], check=True, capture_output=True, timeout=60)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
