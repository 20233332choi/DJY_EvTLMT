import json
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gateway'))
from ev_gateway import EVGateway, TelemetryDatabase, TelemetryStore, make_handler
from tuning import CHANNELS, TuningStream, project, rpm_status
from battery import CHANNELS as BATTERY_CHANNELS, project as battery_project, status as battery_status


class TuningTests(unittest.TestCase):
    def test_battery_samples_keep_times_missing_values_and_expiry(self):
        store, stream = TelemetryStore(), TuningStream(projector=battery_project, status_projector=battery_status)
        for n in range(40):
            store.update({'seq': n+1, 'sample_time_ms': 1000+n*5,
                          'battery_pack_voltage_v': 52+n*.01, 'battery_current_a': 0,
                          'bms_online': True, 'bms_temp_min_c': 25, 'private': 'hidden'}, now=10+n*.005)
            stream.append(store.snapshot(now=10+n*.005))
        rows = stream.read()['samples']
        self.assertEqual([r['time_ms'] for r in rows], list(range(1000, 1200, 5)))
        self.assertEqual(rows[-1]['values']['battery_current_a'], 0)
        self.assertEqual(rows[-1]['values']['bms_temp_min_c'], 25)
        self.assertIsNone(rows[-1]['values']['battery_soc_pct'])
        self.assertIsNone(rows[-1]['values']['bms_temp_max_c'])
        self.assertNotIn('private', json.dumps(rows))
        self.assertNotIn('bms_temp_min_c', project(store.snapshot(now=10.195)))
        self.assertTrue(all(v is None for v in battery_project(store.snapshot(now=14)).values()))

    def test_direct_bms_projection_uses_its_own_values_and_clock(self):
        gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004)
        try:
            gateway.store.update({'sample_time_ms': 100, 'battery_soc_pct': 10,
                                 'bms_power_timing': [1, 1, 0, 1, -1, 10, 10, 10]})
            gateway.bms_configured = True
            gateway.bms_last_monotonic = time.monotonic()
            gateway.bms_state = {'battery_soc_pct': 80, 'battery_current_a': 0, 'bms_temp_min_c': 24}
            gateway._forward_snapshot(forward_live=False, sample_time_ms=200)
            row = gateway.battery.read()['samples'][-1]
            self.assertEqual(row['time_ms'], 200)
            self.assertEqual(row['values']['battery_soc_pct'], 80)
            self.assertEqual(row['values']['battery_current_a'], 0)
            self.assertIsNone(row['values']['battery_pack_voltage_v'])
            gateway.bms_last_monotonic -= 4
            self.assertTrue(all(v is None for v in battery_project(gateway.snapshot()).values()))
        finally:
            gateway.forward_socket.close()

    def test_pid_and_controller_channels_preserve_zero_and_expire(self):
        store = TelemetryStore()
        data = {'vehicle_speed_m_s': 12, 'speed_kmh': 43.2, 'speed_online': True,
                'yaw_error_rad_s': -.1, 'delta_power_kw': -2, 'traction_scale': .75,
                'tv_active': False, 'ed_active': True, 'tqv_internal_online': True,
                'pid_kp': 20, 'pid_ki': 1, 'pid_kd': 0, 'pid_online': True}
        store.update(data, now=10)
        values = project(store.snapshot(now=10))
        for key in ('vehicle_speed_m_s', 'speed_kmh', 'yaw_error_rad_s', 'delta_power_kw',
                    'traction_scale', 'tv_active', 'ed_active', 'pid_kp', 'pid_ki', 'pid_kd'):
            self.assertEqual(values[key], data[key])
        store.update({'battery_soc_pct': 80}, now=11)
        self.assertEqual(project(store.snapshot(now=11))['pid_kp'], 20)
        self.assertIsNone(project(store.snapshot(now=12))['pid_kp'])
        self.assertIsNone(project(store.snapshot(now=12))['tv_active'])
        store.update({'pid_online': False}, now=11.1)
        self.assertIsNone(project(store.snapshot(now=11.1))['pid_kp'])
        for invalid in (-1, True, float('nan'), '20'):
            self.assertIsNone(project({'pid_online': True, 'pid_kp': invalid})['pid_kp'])

    def test_missing_invalid_and_stationary_zero(self):
        self.assertTrue(all(v is None for v in project({}).values()))
        p = {'rpm_left': 0, 'rpm_left_online': True, 'motor_left_ok': True,
             'desired_yaw_rad_s': .3, 'tqv_internal_online': False,
             'yaw_rate_rad_s': float('nan'), 'imu_online': True, 'imu_ok': True,
             'battery_pack_voltage_v': 90}
        v = project(p)
        self.assertEqual(v['rpm_left'], 0)
        self.assertIsNone(v['desired_yaw_rad_s'])
        self.assertIsNone(v['yaw_rate_rad_s'])
        self.assertIsNone(v['motor_left_voltage_v'])

    def test_voltage_ages_independently_and_is_not_pack_voltage(self):
        store = TelemetryStore()
        store.update({'motor_left_voltage_v': 48.2}, now=10)
        store.update({'tps_pct': 20}, now=11)
        self.assertEqual(project(store.snapshot(now=11))['motor_left_voltage_v'], 48.2)
        self.assertIsNone(project(store.snapshot(now=12))['motor_left_voltage_v'])
        self.assertIsNone(project(store.snapshot(now=11))['motor_right_voltage_v'])

    def test_live_cursor_and_overflow(self):
        stream = TuningStream(capacity=3)
        for _ in range(5):
            stream.append({})
        self.assertEqual([s['id'] for s in stream.read()['samples']], [3, 4, 5])
        self.assertTrue(stream.read(1)['truncated'])
        self.assertEqual(stream.read(5)['samples'], [])

    def test_rpm_target_and_reported_age_independently(self):
        store = TelemetryStore()
        store.update({'rpm_left_reported': 7980, 'rpm_left_quality': 'NOISY',
                      'rpm_left_rejected_pct': 99.3, 'rpm_left_target': 2000}, now=10)
        store.update({'tps_pct': 50, 'power_left_kw': 3, 'tqv_internal_online': True}, now=11)
        p = store.snapshot(now=11)
        self.assertEqual(project(p)['rpm_left_reported'], 7980)
        self.assertEqual(project(p)['rpm_left_target'], 2000)
        self.assertIsNone(project(p)['rpm_left'])
        self.assertEqual(rpm_status(p)['rpm_left_quality'], 'NOISY')
        store.update({'rpm_left_reported': 100}, now=12)
        self.assertIsNone(project(store.snapshot(now=12))['rpm_left_target'])
        self.assertIsNone(project(store.snapshot(now=12))['rpm_right_reported'])
        for invalid in (-1, 65536, True, float('nan'), '3200'):
            p = {'rpm_left_target': invalid, 'rpm_left_target_online': True}
            self.assertIsNone(project(p)['rpm_left_target'])
        self.assertEqual(set(rpm_status({'private': 'hidden'})),
                         {'rpm_left_quality', 'rpm_right_quality', 'rpm_left_rejected_pct',
                          'rpm_right_rejected_pct', 'motor_command_mode'})

    def test_unrelated_group_fields_never_create_zero_measurements(self):
        store = TelemetryStore()
        store.update({'power_left_kw': 3, 'lateral_accel_m_s2': .1, 'imu_ok': True}, now=10)
        values = project(store.snapshot(now=10))
        self.assertIsNone(values['desired_yaw_rad_s'])
        self.assertIsNone(values['yaw_rate_rad_s'])
        store.update({'desired_yaw_rad_s': .4}, now=11)
        store.update({'power_left_kw': 4}, now=12)
        self.assertEqual(project(store.snapshot(now=12))['desired_yaw_rad_s'], .4)
        self.assertIsNone(project(store.snapshot(now=13))['desired_yaw_rad_s'])

    def test_history_frozen_pages_and_all_fields_persist(self):
        with tempfile.TemporaryDirectory() as directory:
            db = TelemetryDatabase(Path(directory) / 'test.db')
            try:
                sid = db.start()['recording_session_id']
                for seq in range(5):
                    db.record({'seq': seq, 'desired_yaw_rad_s': .4, 'tqv_internal_online': True,
                               'motor_left_voltage_v': 40+seq, 'motor_left_voltage_online': True,
                               'private': 'must not be shared'})
                first = db.tuning_samples(sid, limit=2)
                db.record({'seq': 6})
                second = db.tuning_samples(sid, first['cursor'], first['through'], limit=2)
                last = db.tuning_samples(sid, second['cursor'], first['through'], limit=2)
                samples = first['samples']+second['samples']+last['samples']
                self.assertEqual(len(samples), 5)
                self.assertEqual(len({s['id'] for s in samples}), 5)
                self.assertFalse(last['more'])
                self.assertEqual(samples[-1]['values']['motor_left_voltage_v'], 44)
                self.assertNotIn('private', json.dumps(samples))
                self.assertIn('private', db.list_samples(sid)['samples'][0]['telemetry'])
            finally:
                db.close()

    def test_shared_http_read_only_and_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=Path(directory)/'test.db')
            server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(gateway))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            base = f'http://127.0.0.1:{server.server_port}'
            def get(path, forwarded=False):
                headers = {'X-Forwarded-For': '203.0.113.1'} if forwarded else {}
                with urllib.request.urlopen(urllib.request.Request(base+path, headers=headers), timeout=3) as r:
                    return r.read()
            try:
                body = json.dumps({'type':'recording_start', 'label':'tuning test'}).encode()
                with urllib.request.urlopen(urllib.request.Request(base+'/api/control', data=body,
                            headers={'Content-Type':'application/json'}), timeout=3) as response:
                    self.assertTrue(json.loads(response.read())['ok'])
                gateway.accept({'seq':10, 'yaw_rate_rad_s':.2, 'imu_ok':True,
                                'desired_yaw_rad_s':.4, 'tqv_internal_online':True})
                live = json.loads(get('/api/tuning/live', True))
                self.assertTrue(live['can_record'])
                self.assertTrue(live['recording_active'])
                self.assertEqual(live['current']['yaw_rate_rad_s'], .2)
                self.assertNotIn('database_path', live)
                self.assertEqual(len(live['channels']), len(CHANNELS))
                gateway.accept({'seq': 11, 'bms_online': True, 'battery_pack_voltage_v': 53,
                                'battery_soc_pct': 75, 'bms_temp_min_c': 22})
                battery = json.loads(get('/api/battery/live', True))
                self.assertEqual(len(battery['channels']), len(BATTERY_CHANNELS))
                self.assertEqual(battery['current']['battery_soc_pct'], 75)
                self.assertEqual(battery['samples'][-1]['values']['bms_temp_min_c'], 22)
                self.assertNotIn('rpm_left', battery['current'])
                self.assertEqual(live['current_status']['rpm_right_quality'], 'NO_DATA')
                self.assertIn('피트 대시보드', get('/pit', True).decode())
                sessions = json.loads(get('/api/tuning/sessions', True))['sessions']
                history = json.loads(get(f"/api/tuning/samples?session_id={sessions[0]['id']}", True))
                self.assertEqual(history['samples'][-1]['values']['desired_yaw_rad_s'], .4)
                with self.assertRaises(urllib.error.HTTPError) as error:
                    get('/api/tuning/samples?session_id=no')
                self.assertEqual(error.exception.code,400)
                error.exception.close()
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(urllib.request.Request(base+'/api/control', data=body,
                        headers={'X-Forwarded-For':'203.0.113.1'}), timeout=3)
                self.assertEqual(error.exception.code,403)
                error.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                gateway.database.close()
                gateway.forward_socket.close()

if __name__ == '__main__':
    unittest.main()
