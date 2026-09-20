import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gateway'))
from ev_gateway import EVGateway, TelemetryDatabase, make_handler
from recording_view import archive_page


class RecordingTests(unittest.TestCase):
    def test_restart_timer_no_synthetic_boundary_samples(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'test.db'
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=path)
            try:
                started = gateway.set_recording(True, 'restart test')
                self.assertEqual(started['recording_sample_count'], 0)
                with patch('ev_gateway.time.monotonic', return_value=gateway.database._clock_anchor+12.5):
                    self.assertAlmostEqual(gateway.database.status()['recording_elapsed_s'], 12.5)
                gateway.accept({'seq': 17, 'rpm_left': 321, 'private': 'must stay local'})
                gateway.database.close()
                gateway.database = TelemetryDatabase(path)
                resumed = gateway.database.status()
                self.assertTrue(resumed['recording_active'])
                self.assertEqual(resumed['recording_started_at_utc'], started['recording_started_at_utc'])
                self.assertEqual(resumed['recording_sample_count'], 1)
                self.assertGreaterEqual(resumed['recording_elapsed_s'], 0)
                stopped = gateway.set_recording(False)
                self.assertEqual(stopped['recording_sample_count'], 1)
                with patch('ev_gateway.time.monotonic', return_value=gateway.database._clock_anchor+500):
                    self.assertEqual(gateway.database.status()['recording_elapsed_s'], stopped['recording_elapsed_s'])
                sample = archive_page(gateway.database, 1)['samples'][0]
                self.assertIsNotNone(sample['received_at_utc'])
                self.assertGreaterEqual(sample['elapsed_s'], -.001)
                self.assertNotIn('must stay local', json.dumps(sample))
                self.assertIn('rpm_left', sample['values'])
                gateway.database.close()
                gateway.database = TelemetryDatabase(path)
                self.assertFalse(gateway.database.status()['recording_active'])
            finally:
                gateway.database.close()
                gateway.forward_socket.close()

    def test_failed_write_does_not_claim_saved_sample(self):
        with tempfile.TemporaryDirectory() as folder:
            db = TelemetryDatabase(Path(folder)/'test.db')
            try:
                db.start()
                db._connection.execute("CREATE TRIGGER fail_record BEFORE INSERT ON telemetry_samples BEGIN SELECT RAISE(ABORT, 'simulated full disk'); END")
                db.record({'seq': 1})
                self.assertTrue(db.status()['recording_error'])
                self.assertEqual(db.status()['recording_sample_count'], 0)
                db._connection.execute('DROP TRIGGER fail_record')
                db.record({'seq': 2})
                self.assertFalse(db.status()['recording_error'])
                self.assertEqual(db.status()['recording_sample_count'], 1)
            finally:
                db.close()

    def test_remote_controls_paging_full_csv_and_vehicle_guard(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(folder)/'test.db')
            server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(gateway))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            base = f'http://127.0.0.1:{server.server_port}'
            def request(path, data=None):
                req = Request(base+path, data=None if data is None else json.dumps(data).encode(),
                              headers={'X-Forwarded-For': '203.0.113.9', 'Content-Type': 'application/json', 'Origin': base})
                with urlopen(req, timeout=5) as response:
                    return response.read()
            try:
                start = json.loads(request('/api/records/control', {'action': 'start', 'label': 'archive'}))
                self.assertTrue(start['recording_active'])
                self.assertNotIn('database_path', start)
                self.assertTrue(json.loads(request('/api/tuning/live'))['can_record'])
                for seq in range(1005):
                    gateway.database.record({'seq': seq, 'private': 'PRIVATE_SECRET', 'tqv_internal_online': True, 'power_left_kw': 3, 'power_right_kw': 4})
                first = json.loads(request('/api/records/samples?session_id=1'))
                self.assertEqual(len(first['samples']), 1000)
                gateway.database.record({'seq': 1005})
                second = json.loads(request(f"/api/records/samples?session_id=1&after={first['cursor']}&through={first['through']}"))
                self.assertEqual(len(second['samples']), 5)
                self.assertFalse(second['more'])
                csv_text = request(f"/api/records/export.csv?session_id=1&through={first['through']}").decode('utf-8-sig')
                rows = list(csv.DictReader(io.StringIO(csv_text)))
                self.assertEqual(len(rows), 1005)
                self.assertEqual(len({r['id'] for r in rows}), 1005)
                self.assertNotIn('PRIVATE_SECRET', csv_text)
                self.assertEqual(float(rows[0]['power_command_total_kw']), 7)
                for path, body in (('/api/control', {'type': 'live_tv'}), ('/api/recording/samples?session_id=1', None)):
                    with self.assertRaises(HTTPError) as denied:
                        request(path, body)
                    self.assertEqual(denied.exception.code, 403)
                    denied.exception.close()
                with self.assertRaises(HTTPError) as invalid:
                    request('/api/records/control', {'action': 'live_tv'})
                self.assertEqual(invalid.exception.code, 400)
                invalid.exception.close()
                stopped = json.loads(request('/api/records/control', {'action': 'stop'}))
                self.assertFalse(stopped['recording_active'])
                self.assertEqual(stopped['recording_sample_count'], 1006)
            finally:
                server.shutdown()
                server.server_close()
                gateway.database.close()
                gateway.forward_socket.close()


if __name__ == '__main__':
    unittest.main()
