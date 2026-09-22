"""Run the actual Rear estimator, frame pairing and histogram code on the host."""
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
from host_compiler import compiler

FIRMWARE=Path(__file__).resolve().parents[1]/'firmware'
REAR=FIRMWARE/'stm32_rear_binary'
FRONT=FIRMWARE/'stm32_front_synced'

class BoardTimeSyncTests(unittest.TestCase):
    def test_protocol_gating_cookies_and_sensor_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            shutil.copyfile(REAR/'Core/Src/board_time_sync.c',folder/'board_time_sync.c')
            (folder/'main.h').write_text('/* HAL mocked by harness. */')
            exe=folder/'protocol.exe'
            source=Path(__file__).with_name('board_time_protocol_harness.c')
            result=subprocess.run(compiler()+['-std=gnu11','-O2','-Wall','-Werror','-I',str(folder),
                '-I',str(REAR/'Core/Inc'),str(source),'-o',str(exe),'-lm'],capture_output=True,timeout=180)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            result=subprocess.run([str(exe)],capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

    def test_shared_sources_identical(self):
        for name in ('Inc/vehicle_clock.h','Inc/board_time_sync.h','Inc/time_sync_model.h',
                     'Src/vehicle_clock.c','Src/board_time_sync.c'):
            self.assertEqual((REAR/'Core'/name).read_bytes(),(FRONT/'Core'/name).read_bytes(),name)

    def test_offset_drift_wrap_reboot_missing_and_delayed_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(__file__).with_name('board_time_sync_harness.c')
            exe=Path(tmp)/'sync.exe'
            result=subprocess.run(compiler()+['-std=gnu11','-O2','-Wall','-Werror','-I',str(REAR/'Core/Inc'),str(source),'-o',str(exe),'-lm'],capture_output=True,timeout=180)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            result=subprocess.run([str(exe)],capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))

if __name__=='__main__':unittest.main()
