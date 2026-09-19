"""Actual STM RPM C with simulated capture/ticks. Never opens a board."""
import pathlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from host_compiler import compiler

ROOT = pathlib.Path(__file__).resolve().parents[1]
_candidates = (ROOT.parent / 'DJY_TQV', ROOT.parents[1] / 'DJY_TQV',
               ROOT.parents[1] / 'DJY_TQV' / 'DJY_TQV')
BASE = pathlib.Path(os.environ.get('DJY_TQV_REPOSITORY',
    str(next((p for p in _candidates if (p / '.git').exists()), _candidates[0]))))
OVERLAY = ROOT / 'firmware/stm32_rear_uart/Core'
REVISION = re.search(r"\$revision = '([0-9a-f]+)'",
                     (ROOT / 'scripts/build_stm32_rear_uart.ps1').read_text(encoding='utf-8'))[1]


def pinned(relative):
    return subprocess.run(['git', '-C', str(BASE), 'show',
                           f'{REVISION}:firmware/rear/Core/{relative}'],
                          check=True, capture_output=True, timeout=10).stdout


class StmRpmTests(unittest.TestCase):
    def compile_and_run(self, baseline=False):
        with tempfile.TemporaryDirectory(prefix='djy-rpm-test-') as tmp:
            folder = pathlib.Path(tmp)
            for name in ('rpm_sensor.h', 'vehicle_params.h', 'common_types.h', 'filters.h', 'board_config.h'):
                local = OVERLAY / 'Inc' / name
                data = local.read_bytes() if not baseline and local.exists() else pinned('Inc/' + name)
                (folder / name).write_bytes(data)
            source = pinned('Src/rpm_sensor.c') if baseline else (OVERLAY / 'Src/rpm_sensor.c').read_bytes()
            (folder / 'rpm_sensor.c').write_bytes(source)
            (folder / 'main.h').write_text('/* PC HAL supplied by the test harness */\n')
            exe = folder / 'rpm_test.exe'
            command = compiler()+['-std=c11', '-O2', '-Wall', '-Wextra',
                       '-I', str(folder), str(ROOT / 'tests/stm32_rpm_harness.c'), '-o', str(exe), '-lm']
            command.append('-DBASELINE' if baseline else '-Werror')
            built = subprocess.run(command, capture_output=True, text=True,
                                   encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', timeout=15)

    def test_pinned_original_reproduces_false_rpm(self):
        result = self.compile_and_run(baseline=True)
        self.assertNotEqual(result.returncode, 0, 'Original must fail the same no-false-RPM check')
        self.assertRegex(result.stdout, r'R=79\d\d fresh=1')
        self.assertIn('!RPM_IsFresh()', result.stderr)

    def test_fixed_capture_filter(self):
        result = self.compile_and_run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('L=0 R=0 fresh=0', result.stdout)
        self.assertIn('scenarios PASS', result.stdout)

    def test_invalid_zero_cannot_bypass_pit_activity_guard(self):
        main = (OVERLAY / 'Src/main.c').read_text(encoding='utf-8')
        gate = main[main.index('if (CAN_GetPendingPitConfig'):main.index('ControlSettings_ApplyPitConfig')]
        self.assertIn('!RPM_HasRecentActivity()', gate)
        self.assertIn('CAN_IsSensorFresh()', gate)
        self.assertIn('TPS_to_Fraction', gate)


if __name__ == '__main__':
    unittest.main()
