"""High-rate columnar batches -> every DB row -> stable pages/full downloads."""
import csv
import io
import json
import sqlite3
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gateway'))
from ev_gateway import EVGateway, TelemetryDatabase, make_handler


class HighRateRecordingTests(unittest.TestCase):
    def test_full_session_survives_batches_retry_paging_and_export(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=Path(folder)/'test.db')
            gateway.forward_socket.close()
            gateway.forward_socket = Mock()
            server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(gateway))
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            base = f'http://127.0.0.1:{server.server_port}'

            def get(path, forwarded=False):
                headers = {'X-Forwarded-For': '203.0.113.1'} if forwarded else {}
                with urllib.request.urlopen(urllib.request.Request(base+path, headers=headers), timeout=10) as response:
                    return response.read().decode('utf-8-sig')

            try:
                sid = gateway.database.start()['recording_session_id']
                statements = []
                gateway.database._connection.set_trace_callback(statements.append)
                for first in range(1, 1101, 64):
                    rows = [{'seq': seq, 'timestamp_ms': seq*5,
                             'sample_kind': 'rear' if seq%2 else 'bms',
                             'yaw_rate_rad_s': seq/1000, 'imu_ok': True,
                             'desired_yaw_rad_s': .4, 'tqv_internal_online': True,
                             'private_field': {'array': [seq, None, False]},
                             'battery_power_kw': -9.1, 'bms_online': True}
                            for seq in range(first, min(first+64, 1101))]
                    data = {'stream_id': '0123456789abcdef', 'sent_ms': rows[-1]['timestamp_ms']+5,
                            'columns': list(rows[0]), 'samples': [list(row.values()) for row in rows]}
                    reply = gateway.relay_exchange(data)
                    self.assertEqual(reply['accepted_samples'], len(rows))
                    self.assertEqual(gateway.relay_exchange(data)['accepted_samples'], 0)
                self.assertEqual(gateway.database.status()['recording_sample_count'], 1100)
                self.assertEqual(sum(s == 'COMMIT' for s in statements), 18)
                self.assertEqual(gateway.forward_socket.sendto.call_count, 18)
                live = gateway.tuning.read()['samples']
                self.assertEqual(len(live), 1100)
                self.assertEqual([live[i+1]['time_ms']-live[i]['time_ms'] for i in range(1099)], [5]*1099)

                first = json.loads(get(f'/api/recording/samples?session_id={sid}&after=0&limit=1000'))
                self.assertTrue(first['more'])
                gateway.database.record({'seq':1101, 'private_field': 'arrived during browsing'})
                last = json.loads(get(f'/api/recording/samples?session_id={sid}&after={first["cursor"]}&through={first["through"]}'))
                self.assertEqual(len(first['samples'])+len(last['samples']),1100)
                self.assertFalse(last['more'])
                self.assertEqual(last['samples'][-1]['telemetry']['private_field']['array'], [1100, None, False])

                full = [json.loads(row) for row in get(f'/api/recording/export?session_id={sid}').splitlines()]
                self.assertEqual(len(full), 1101)
                self.assertEqual([r['telemetry']['seq'] for r in full], list(range(1,1102)))
                raw_csv = list(csv.DictReader(io.StringIO(get(f'/api/recording/export?session_id={sid}&format=csv'))))
                self.assertEqual(len(raw_csv),1101)
                self.assertEqual(json.loads(raw_csv[1099]['payload_json'])['private_field']['array'][0],1100)
                shared_csv = get(f'/api/tuning/export?session_id={sid}', True)
                self.assertNotIn('private_field', shared_csv)
                self.assertNotIn('payload_json', shared_csv)
                self.assertEqual(len(list(csv.DictReader(io.StringIO(shared_csv)))),1101)
                with self.assertRaises(urllib.error.HTTPError) as denied:
                    get(f'/api/recording/export?session_id={sid}', True)
                self.assertEqual(denied.exception.code,403)
                denied.exception.close()
                bms_csv = list(csv.DictReader(io.StringIO(get(f'/api/battery/export?session_id={sid}', True))))
                self.assertEqual(len(bms_csv), 1101)
                self.assertIn('battery_power_w', bms_csv[0])
                self.assertNotIn('payload_json', bms_csv[0])
            finally:
                server.shutdown(); server.server_close(); worker.join()
                gateway.database.close()

    def test_sql_error_rejects_batch_ack_and_retry_saves_all_rows(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=Path(folder)/'test.db')
            db = gateway.database
            try:
                sid = db.start()['recording_session_id']
                db._connection.execute("CREATE TRIGGER fail_second BEFORE INSERT ON telemetry_samples WHEN NEW.sequence=2 BEGIN SELECT RAISE(ABORT, 'simulated write failure'); END")
                data = {'stream_id': '0123456789abcdef', 'sent_ms': 30,
                        'columns': ['seq', 'timestamp_ms'], 'samples': [[1, 10], [2, 20]]}
                with self.assertRaises(sqlite3.IntegrityError):
                    gateway.relay_exchange(data)
                self.assertEqual(db.status()['recording_sample_count'], 0)
                self.assertIsNone(db.status()['recording_last_received_at_utc'])
                self.assertTrue(db.status()['recording_error'])
                self.assertEqual(db.tuning_samples(sid)['samples'], [])
                self.assertEqual(gateway.tuning.read()['samples'], [])
                self.assertEqual(gateway.battery.read()['samples'], [])
                db._connection.execute('DROP TRIGGER fail_second')
                reply = gateway.relay_exchange(data)
                self.assertEqual(reply['ack_seq'], 2)
                self.assertEqual(reply['accepted_samples'], 2)
                self.assertEqual(db.status()['recording_sample_count'], 2)
                self.assertEqual(len(gateway.tuning.read()['samples']), 2)
                self.assertEqual(len(gateway.battery.read()['samples']), 2)
                self.assertFalse(db.status()['recording_error'])
                self.assertEqual(gateway.relay_exchange(data)['accepted_samples'], 0)
            finally:
                db.close()
                gateway.forward_socket.close()

    def test_final_commit_failure_and_udp_failure_do_not_duplicate_graphs(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=Path(folder)/'test.db')
            gateway.forward_socket.close()
            gateway.forward_socket = Mock()
            db = gateway.database
            try:
                db.start()
                db._connection.execute("CREATE TRIGGER fail_commit BEFORE UPDATE ON measurement_sessions BEGIN SELECT RAISE(ABORT, 'simulated final write failure'); END")
                data = {'stream_id': '0123456789abcdef', 'sent_ms': 30,
                        'columns': ['seq', 'timestamp_ms'], 'samples': [[1, 10], [2, 20]]}
                with self.assertRaises(sqlite3.IntegrityError):
                    gateway.telemetry_exchange(data, 'USB')
                self.assertTrue(db.status()['recording_error'])
                self.assertEqual(gateway.tuning.read()['samples'], [])
                self.assertEqual(db.status()['recording_sample_count'], 0)
                db._connection.execute('DROP TRIGGER fail_commit')
                gateway.forward_socket.sendto.side_effect = OSError('UDP destination unavailable')
                self.assertEqual(gateway.telemetry_exchange(data, 'USB')['ack_seq'], 2)
                self.assertEqual(gateway.relay_exchange(data)['accepted_samples'], 0)
                self.assertEqual(len(gateway.tuning.read()['samples']), 2)
                self.assertEqual(len(gateway.battery.read()['samples']), 2)
                self.assertEqual(db.status()['recording_sample_count'], 2)
            finally:
                db.close()

    def test_usb_udp_and_relay_share_identity_and_fill_missing_samples(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=Path(folder)/'test.db')
            gateway.forward_socket.close()
            gateway.forward_socket = Mock()
            db = gateway.database
            def batch(sequences, sent):
                return {'stream_id': '0123456789abcdef', 'sent_ms': sent,
                        'samples': [{'seq': n, 'timestamp_ms': n*5, 'imu_ok': True,
                                     'yaw_rate_rad_s': n/1000, 'tqv_internal_online': True}
                                    for n in sequences]}
            try:
                sid = db.start()['recording_session_id']
                for first in range(1, 1001, 32):
                    seqs = list(range(first, min(first+32, 1001)))
                    data = batch(seqs, seqs[-1]*5)
                    # USB delivers most samples, UDP fills one hole, HTTPS fills the other.
                    partial = batch(seqs[2:], seqs[-1]*5)
                    gateway.telemetry_exchange(partial, 'USB')
                    row = dict(data['samples'][0], stream_id=data['stream_id'])
                    self.assertTrue(gateway.accept(row, transport='WIFI_UDP'))
                    self.assertFalse(gateway.accept(row, transport='USB'))
                    result = gateway.relay_exchange(data)
                    self.assertEqual(result['accepted_samples'], 1)
                    self.assertEqual(result['ack_seq'], seqs[-1])
                self.assertEqual(db.status()['recording_sample_count'], 1000)
                live = gateway.tuning.read()['samples']
                self.assertEqual(len(live), 1000)
                self.assertEqual(len(gateway.battery.read()['samples']), 1000)
                self.assertEqual(sorted(s['values']['yaw_rate_rad_s'] for s in live), [n/1000 for n in range(1,1001)])
                self.assertEqual(gateway.snapshot()['seq'], 1000) # Delayed backfill cannot rewind live state.
                retry = gateway.telemetry_exchange(batch([1,2], 10), 'USB')
                self.assertEqual(retry, {'ack_seq': 2, 'accepted_samples': 0})
                self.assertEqual(db.status()['recording_sample_count'], 1000)
                self.assertEqual(gateway.relay_samples.received[('EV','0123456789abcdef')], [(1,1000)])
                times = sorted(s['time_ms'] for s in db.tuning_samples(sid)['samples'])
                self.assertEqual([b-a for a,b in zip(times,times[1:])], [5]*999)
            finally:
                db.close()

    def test_failed_db_batch_is_rolled_back(self):
        with tempfile.TemporaryDirectory() as folder:
            db = TelemetryDatabase(Path(folder)/'test.db')
            try:
                sid = db.start()['recording_session_id']
                with self.assertRaises(RuntimeError):
                    with db.batch():
                        db.record({'seq': 1})
                        raise RuntimeError('disk or processing failure')
                self.assertEqual(db.status()['recording_sample_count'],0)
                self.assertEqual(db.tuning_samples(sid)['samples'],[])
                db.record({'seq':2})
                self.assertEqual(db.status()['recording_sample_count'],1)
            finally:
                db.close()


if __name__ == '__main__':
    unittest.main()
