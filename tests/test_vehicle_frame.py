"""Execute the deployed project's real IMU parser and TV control on the host."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from host_compiler import compiler

PROJECT = Path(os.environ.get('DJY_STM_PROJECT', str(Path(__file__).resolve().parents[1]/'firmware/stm32_rear_binary')))

class VehicleFrameTests(unittest.TestCase):
    def test_flu_sensor_and_corrective_torque(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name in ('imu_sensor', 'torque_vectoring', 'pid_controller', 'electronic_diff'):
                shutil.copyfile(PROJECT/'Core/Src'/f'{name}.c', folder/f'{name}.c')
                shutil.copyfile(PROJECT/'Core/Inc'/f'{name}.h', folder/f'{name}.h')
            for name in ('vehicle_params.h', 'common_types.h', 'filters.h', 'vehicle_clock.h', 'timing_diag.h'):
                shutil.copyfile(PROJECT/'Core/Inc'/name, folder/name)
            (folder/'board_config.h').write_text('/* No board I/O in host test. */')
            (folder/'main.h').write_text('/* HAL stubs precede source include. */')
            source = Path(__file__).with_name('vehicle_frame_harness.c')
            exe = folder/'frame.exe'
            result = subprocess.run(compiler()+['-std=gnu11', '-O1', '-I', str(folder),
                str(source), str(folder/'torque_vectoring.c'), str(folder/'pid_controller.c'),
                str(folder/'electronic_diff.c'), '-o', str(exe), '-lm'], capture_output=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            result = subprocess.run([str(exe)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

if __name__ == '__main__': unittest.main()
