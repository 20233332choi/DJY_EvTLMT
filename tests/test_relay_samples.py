import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'gateway'))
from relay_samples import RelaySamples
from ev_gateway import EVGateway


def batch(first=1, count=4, sent=1000, start=200, stream='0123456789abcdef'):
    return {'stream_id': stream, 'sent_ms': sent, 'dropped_samples': 0,
            'samples': [{'seq': first+i, 'timestamp_ms': start+200*i,
                         'stm_online': True, 'imu_online': True, 'imu_ok': True,
                         'tqv_internal_online': True, 'yaw_rate_rad_s': i/10,
                         'desired_yaw_rad_s': .5, 'yaw_error_rad_s': .5-i/10,
                         'tv_active': True, 'ed_active': False}
                        for i in range(count)]}


class RelaySampleTests(unittest.TestCase):
    def test_capture_spacing_survives_batching_and_network_jitter(self):
        receiver = RelaySamples()
        key, rows, ack, clock = receiver.prepare(batch(), 'EV', now_ms=100000)
        self.assertEqual([r['sample_time_ms'] for r in rows], [99200, 99400, 99600, 99800])
        receiver.commit(key, ack, clock)
        _, rows, _, _ = receiver.prepare(batch(5, sent=1800, start=1000), 'EV', now_ms=100999)
        self.assertEqual([r['sample_time_ms'] for r in rows], [100000, 100200, 100400, 100600])

    def test_retries_overlap_and_reboot(self):
        r = RelaySamples()
        key, rows, ack, clock = r.prepare(batch(), 'EV', 100000)
        r.commit(key, ack, clock)
        self.assertEqual(r.prepare(batch(), 'EV', 100100)[1], [])
        overlap = batch(3, sent=1600, start=600)
        key, rows, ack, clock = r.prepare(overlap, 'EV', 100600)
        self.assertEqual([row['seq'] for row in rows], [5, 6])
        r.commit(key, ack, clock)
        self.assertEqual(len(r.prepare(batch(stream='abcdef0123456789'), 'EV', 101000)[1]), 4)

    def test_millis_wrap_preserves_spacing(self):
        r = RelaySamples()
        first = batch(1, count=1, sent=0xffffff00, start=0xfffffe00)
        key, _, ack, clock = r.prepare(first, 'EV', 100000)
        r.commit(key, ack, clock)
        _, rows, _, _ = r.prepare(batch(2, count=1, sent=256, start=0), 'EV', 100600)
        self.assertEqual(rows[0]['sample_time_ms'], 100256)
        self.assertEqual(rows[0]['sample_age_at_receive_ms'], 256)

    def test_invalid_batch_does_not_partially_advance(self):
        for mutation in ('duplicate', 'backwards_time', 'future', 'too_many', 'bool_seq'):
            r = RelaySamples(); data = batch()
            if mutation == 'duplicate': data['samples'][3]['seq'] = 1
            if mutation == 'backwards_time': data['samples'][3]['timestamp_ms'] = 0
            if mutation == 'future': data['samples'][3]['timestamp_ms'] = 1001
            if mutation == 'too_many': data['samples'].append(data['samples'][-1])
            if mutation == 'bool_seq': data['samples'][0]['seq'] = True
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                r.prepare(data, 'EV', 100000)
            self.assertEqual(len(r.streams), 0)

    def test_gateway_records_every_sample_once_and_preserves_time(self):
        with tempfile.TemporaryDirectory() as directory:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004,
                                database_path=pathlib.Path(directory)/'samples.db')
            gateway.forward_socket.close(); gateway.forward_socket = Mock()
            try:
                sid = gateway.database.start()['recording_session_id']
                reply = gateway.relay_exchange(batch())
                self.assertEqual((reply['ack_seq'], reply['accepted_samples']), (4, 4))
                retry = gateway.relay_exchange(batch())
                self.assertEqual(retry['accepted_samples'], 0)
                live = gateway.tuning.read()['samples']
                stored = gateway.database.tuning_samples(sid)['samples']
                self.assertEqual(len(live), 4); self.assertEqual(len(stored), 4)
                self.assertEqual([s['values']['yaw_rate_rad_s'] for s in live], [0, .1, .2, .3])
                self.assertEqual([s['time_ms'] for s in live], [s['time_ms'] for s in stored])
                self.assertEqual([live[i+1]['time_ms']-live[i]['time_ms'] for i in range(3)], [200]*3)
                self.assertEqual(gateway.snapshot()['seq'], 4)
            finally:
                gateway.database.close()

    def test_delayed_samples_retained_but_not_live(self):
        gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004)
        gateway.forward_socket.close(); gateway.forward_socket = Mock()
        gateway.relay_exchange(batch(sent=5000))
        live = gateway.snapshot()
        self.assertFalse(live['online']); self.assertFalse(live['stm_online'])
        self.assertFalse(live['imu_online'])
        samples = gateway.tuning.read()['samples']
        self.assertEqual([s['values']['yaw_rate_rad_s'] for s in samples], [0, .1, .2, .3])

    def test_bad_last_sample_ingests_nothing(self):
        gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004)
        gateway.forward_socket.close(); gateway.forward_socket = Mock()
        data = batch(); data['samples'][-1] = None
        with self.assertRaises(ValueError): gateway.relay_exchange(data)
        self.assertEqual(gateway.tuning.read()['samples'], [])


if __name__ == '__main__': unittest.main()
