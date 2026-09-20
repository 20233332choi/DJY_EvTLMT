import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gateway'))
from battery import project, status, with_power
from ev_gateway import EVGateway, TelemetryStore
from vehicle_geometry import speed_kmh, with_verified_speed


class BatteryDashboardTests(unittest.TestCase):
    def test_power_units_zero_and_signed_current(self):
        for current, watts in ((100, 7200), (0, 0), (-10, -720)):
            store = TelemetryStore()
            store.update({'battery_pack_voltage_v': 72, 'battery_current_a': current,
                          'battery_power_kw': 999, 'bms_online': True}, now=10)
            values = project(store.snapshot(now=10))
            self.assertEqual(values['battery_power_w'], watts)
            self.assertEqual(values['battery_power_kw'], watts / 1000)

    def test_missing_partial_invalid_stale_and_faulted(self):
        store = TelemetryStore()
        store.update({'battery_pack_voltage_v': 72, 'bms_online': True}, now=10)
        values = project(store.snapshot(now=10))
        self.assertIsNone(values['battery_current_a'])
        self.assertIsNone(values['battery_power_w'])
        self.assertIsNone(status(store.snapshot(now=10))['bms_fault'])
        store.update({'battery_current_a': 100, 'bms_online': True}, now=11)
        self.assertIsNone(project(store.snapshot(now=11))['battery_power_w'])
        store.update({'battery_pack_voltage_v': 72, 'battery_current_a': 100,
                      'bms_fault': True, 'bms_online': True, 'bms_ok': False}, now=12)
        self.assertEqual(project(store.snapshot(now=12))['battery_power_w'], 7200)
        self.assertTrue(status(store.snapshot(now=12))['bms_fault'])
        store.update({'tps_pct': 10}, now=14)
        self.assertEqual(project(store.snapshot(now=14))['battery_power_w'], 7200)
        self.assertIsNone(project(store.snapshot(now=15.1))['battery_power_w'])
        for invalid in (None, True, float('nan'), float('inf')):
            self.assertIsNone(with_power({'battery_pack_voltage_v': 72,
                                         'battery_current_a': invalid})['battery_power_w'])

    def test_recording_retains_bms_and_commands_without_fake_motor_measurements(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(folder)/'test.db')
            try:
                gateway.set_recording(True, 'battery test')
                gateway.accept({'seq': 1, 'battery_pack_voltage_v': 72, 'battery_current_a': 100,
                                'bms_online': True, 'bms_cell_voltages_v': [3.7, 3.71],
                                'bms_fault': False, 'bms_cell_count': 2,
                                'tqv_internal_online': True, 'power_left_kw': 2, 'power_right_kw': 3,
                                'delta_power_kw': 1, 'private_secret': 'must-not-be-in-public-projection'})
                sid = gateway.database.status()['recording_session_id']
                history = gateway.database.tuning_samples(sid, projector=project, status_projector=status)
                row = history['samples'][-1]
                self.assertEqual(row['values']['battery_power_w'], 7200)
                self.assertEqual(row['values']['power_command_total_kw'], 5)
                self.assertEqual(row['status']['bms_cell_voltages_v'], [3.7, 3.71])
                self.assertNotIn('private_secret', str(row))
                self.assertNotIn('motor_left_power_w', row['values'])
                self.assertEqual(gateway.battery.read()['samples'][-1]['values'], row['values'])
                gateway.accept({'seq': 2, 'battery_pack_voltage_v': 72, 'bms_online': True})
                sql_row = gateway.database._connection.execute(
                    'SELECT battery_pack_voltage_v,battery_power_kw FROM telemetry_samples ORDER BY id DESC LIMIT 1').fetchone()
                self.assertEqual(sql_row[0], 72)
                self.assertIsNone(sql_row[1])
            finally:
                gateway.database.close()
                gateway.forward_socket.close()

    def test_confirmed_geometry_and_preserved_board_speed(self):
        self.assertAlmostEqual(speed_kmh(800, 1200), 21.2057504117)
        self.assertEqual(speed_kmh(0, 0), 0)
        data = {'rpm_left': 800, 'rpm_right': 1200, 'motor_left_ok': True,
                'motor_right_ok': True, 'vehicle_speed_m_s': 99, 'speed_kmh': 88}
        result = with_verified_speed(data)
        self.assertAlmostEqual(result['speed_kmh'], 21.2057504117)
        self.assertEqual(result['vehicle_speed_m_s'], 99)
        self.assertEqual(result['speed_reported_kmh'], 88)
        self.assertEqual(with_verified_speed({**data, 'motor_left_ok': False})['speed_kmh'], 88)
        store = TelemetryStore()
        store.update(data, now=10)
        from tuning import project as tv_project
        self.assertAlmostEqual(tv_project(store.snapshot(now=10))['speed_kmh'], 21.206, places=3)
        self.assertIsNone(tv_project(store.snapshot(now=12))['speed_kmh'])


if __name__ == '__main__':
    unittest.main()
