"""Actual IMU C with simulated UART input; never opens a board."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from host_compiler import compiler

try:
    from .test_stm32_rpm import OVERLAY, ROOT, pinned
except ImportError:  # Direct execution: python tests/test_stm32_imu.py
    from test_stm32_rpm import OVERLAY, ROOT, pinned


class StmImuTests(unittest.TestCase):
    def test_passive_baud_probe_and_validity(self):
        with tempfile.TemporaryDirectory(prefix='djy-imu-test-') as tmp:
            folder = pathlib.Path(tmp)
            for name in ('imu_sensor.h', 'vehicle_params.h', 'common_types.h',
                         'filters.h', 'board_config.h'):
                local = OVERLAY / 'Inc' / name
                (folder / name).write_bytes(local.read_bytes() if local.exists()
                                           else pinned('Inc/' + name))
            (folder / 'imu_sensor.c').write_bytes((OVERLAY / 'Src/imu_sensor.c').read_bytes())
            (folder / 'main.h').write_text('/* HAL supplied by harness */\n')
            exe = folder / 'imu_test.exe'
            result = subprocess.run(compiler()+['-std=c11', '-O2', '-Wall', '-Wextra',
                                     '-Werror', '-I', str(folder),
                                     str(ROOT / 'tests/stm32_imu_harness.c'),
                                     '-o', str(exe), '-lm'],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            for scenario in ('9600', '115200', '230400', 'corrupt', 'gyro_only',
                             'silent', 'uart_error', 'init_error'):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(exe), scenario], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('PASS', result.stdout)
