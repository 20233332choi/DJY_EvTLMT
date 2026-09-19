import json
import sqlite3
import sys
import tempfile
import threading
import unittest
from contextlib import closing
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gateway'))
from ev_gateway import EVGateway, TelemetryStore
from stm_serial import RearStatusParser, rear_records
from tuning import project, rpm_status


LINE = ('L=0 R=7980 cap=3/71440741 glt=1/70945769 tps=757 pct=0 idle=868 '
        'imu=0 sas=16379 yaw=0 lat=0 lon=0 ax=0 ay=0 az=0 '
        'vs=25176 dy=100 ye=-20 dp=1500 pl=2000 pr=3500 tva=1 eda=0 tr=750 '
        'ctl=0/0 req=0 lim=100 app=0 tv=1 ed=0 fault=5 dac=1116/1116 '
        'sas=16379 imu=0 urx=0 uerr=0 ipk=0 bad=0 rs=0 future=42\r\n').encode()


class FakeStream:
    def __init__(self, chunks, stop):
        self.chunks = iter(chunks)
        self.stop = stop
        self.opened = self.closed = False

    def open(self):
        assert self.dtr is False and self.rts is False
        assert self.port == 'COM13'
        self.opened = True

    def __enter__(self):
        assert self.opened
        return self

    def __exit__(self, *args):
        self.closed = True

    def read_until(self, expected, size):
        assert expected == b'\n' and size == 2049
        try:
            return next(self.chunks)
        except StopIteration:
            self.stop.set()
            return b''

    def write(self, data):
        raise AssertionError('Rear USB must never receive a command')


class RearSerialTests(unittest.TestCase):
    def test_stm_rpm_validity_prevents_false_healthy_zero(self):
        parser = RearStatusParser()
        parser.parse(b'L=0 R=0 cap=0/0 glt=0/0 tps=868 pct=0 idle=868 rv=0/0')
        p = parser.parse(b'L=0 R=0 cap=1/1 glt=0/0 tps=868 pct=0 idle=868 rv=0/0')
        self.assertFalse(p['rpm_left_online'])
        self.assertFalse(p['motor_right_ok'])
        self.assertIsNone(project(p)['rpm_right'])
        self.assertEqual(project(p)['rpm_right_reported'], 0)
        p = parser.parse(b'L=951 R=951 cap=3/3 glt=0/0 tps=868 pct=0 idle=868 rv=1/1')
        self.assertTrue(p['motor_left_ok'])
        self.assertEqual(project(p)['rpm_right'], 951)
        p = parser.parse(b'L=0 R=951 cap=4/4 glt=1/0 tps=868 pct=0 idle=868 rv=0/1')
        self.assertFalse(p['rpm_left_online'])
        self.assertTrue(p['rpm_right_online'])
        for value in (b'1', b'1/2', b'1/0/1'):
            self.assertIsNone(parser.parse(b'L=0 R=0 cap=0/0 glt=0/0 tps=868 pct=0 idle=868 rv=' + value))

    def test_real_noisy_record_preserved_but_not_plotted(self):
        parser = RearStatusParser()
        p = parser.parse(LINE)
        self.assertEqual(p['stm_raw_line'], LINE.decode().strip())
        self.assertEqual(p['stm_fields']['future'], '42')
        self.assertEqual(p['rpm_right'], 7980)
        self.assertEqual(p['rpm_right_quality'], 'NOISY')
        self.assertFalse(p['speed_online'])
        self.assertFalse(p['sas_ok'])
        self.assertFalse(p['imu_ok'])
        self.assertEqual(p['vehicle_speed_m_s'], 25.176)
        self.assertEqual(p['desired_yaw_rad_s'], .1)
        self.assertEqual(p['yaw_error_rad_s'], -.02)
        self.assertEqual(p['delta_power_kw'], 1.5)
        self.assertEqual(p['power_left_kw'], 2)
        self.assertEqual(p['power_right_kw'], 3.5)
        self.assertEqual(p['traction_scale'], .75)
        self.assertTrue(p['tv_active'])
        self.assertFalse(p['ed_active'])
        self.assertIsNone(project(p)['rpm_right'])
        self.assertEqual(project(p)['rpm_right_reported'], 7980)
        self.assertIsNone(project(p)['rpm_right_target'])
        self.assertEqual(rpm_status(p)['motor_command_mode'], 'POWER')
        self.assertIsNone(project(p)['yaw_rate_rad_s'])
        self.assertIsNone(project(p)['sas_deg'])
        self.assertIsNone(project(p)['motor_left_voltage_v'])
        self.assertEqual(project(p)['desired_yaw_rad_s'], .1)

    def test_counter_deltas_recover_from_old_noise_and_reject_new_noise(self):
        parser = RearStatusParser()
        parser.parse(b'L=100 R=100 cap=100/100 glt=99/99 tps=868 pct=0 idle=868')
        p = parser.parse(b'L=110 R=110 cap=110/110 glt=99/99 tps=868 pct=0 idle=868')
        self.assertTrue(p['motor_right_ok'])
        p = parser.parse(b'L=110 R=7980 cap=111/200 glt=99/188 tps=868 pct=0 idle=868')
        self.assertEqual(p['rpm_right_quality'], 'NOISY')
        self.assertAlmostEqual(p['rpm_right_rejected_pct'], 98.9)
        self.assertEqual(project(p)['rpm_right_reported'], 7980)
        p = parser.parse(b'L=0 R=0 cap=0/0 glt=0/0 tps=868 pct=0 idle=868')
        self.assertFalse(p['motor_left_ok'])
        self.assertFalse(p['motor_right_ok'])

    def test_target_rpm_requires_explicit_field_never_tps_or_power(self):
        parser = RearStatusParser()
        line = LINE.replace(b'pct=0', b'pct=100')
        p = parser.parse(line)
        self.assertIsNone(project(p)['rpm_left_target'])
        p = parser.parse(line.rstrip() + b' rpm_target_l=0 rpm_target_r=3200\n')
        self.assertEqual(project(p)['rpm_left_target'], 0)
        self.assertEqual(project(p)['rpm_right_target'], 3200)
        self.assertEqual(p['motor_command_mode'], 'RPM')
        for bad in (b'-1', b'65536'):
            p = parser.parse(line.rstrip() + b' rpm_target_l=' + bad + b'\n')
            self.assertIsNone(project(p)['rpm_left_target'])

    def test_short_and_team_variants_do_not_fabricate_missing_fields(self):
        p = RearStatusParser().parse(b'L=0 R=0 cap=0/0 glt=0/0 dsy=0/0 tps=896 pct=0 idle=897 can=14369/0/00000000 imu=12859/0/282887/0')
        self.assertIsNotNone(p)
        self.assertNotIn('desired_yaw_rad_s', p)
        self.assertFalse(p['tqv_internal_online'])
        self.assertFalse(p['imu_ok'])
        self.assertFalse(p['tps_online'])
        self.assertEqual(p['stm_fields']['imu'], '12859/0/282887/0')

    def test_explicit_sensor_validity_and_timeout(self):
        line = LINE.replace(b'imu=0', b'imu=1').replace(b'yaw=0', b'yaw=-37').replace(b'fault=5', b'fault=3')
        p = RearStatusParser().parse(line.rstrip() + b' sas_ok=1\r\n')
        self.assertTrue(p['imu_ok'])
        self.assertEqual(p['yaw_rate_rad_s'], -.037)
        self.assertTrue(p['sas_ok'])
        p = RearStatusParser().parse(LINE.replace(b'fault=5', b'fault=1'))
        self.assertFalse(p['front_sensor_online'])
        self.assertFalse(p['tps_ok'])
        self.assertTrue(p['stm_online'])

    def test_invalid_and_fragmented_records(self):
        parser = RearStatusParser()
        for line in (b'# boot', b'R=4', b'L=0 R=0 cap=0/0 glt=0/0 tps=9999 pct=0 idle=868',
                     LINE.replace(b'cap=3/71440741', b'cap=3'), LINE + b'\xff', b'L=' + b'1'*2100):
            self.assertIsNone(parser.parse(line))
        stop = threading.Event()
        stream = FakeStream([b'L=' + b'9'*2047, b' tail\n', LINE[:73], b'', LINE[73:]], stop)
        records = list(rear_records(stream, stop))
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]['stm_raw_line'], LINE.decode().strip())

    def test_usb_loop_is_passive_and_records_all_values(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.db'
            g = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=path)
            g.input_mode, g.input_port = 'STM_USB', 'COM13'
            stream = FakeStream([LINE[:40], LINE[40:]], g.stop_event)
            try:
                self.assertFalse(g.database.status()['recording_active'])
                g.set_recording(True, 'UNIT TEST')
                with patch('serial.Serial', return_value=stream):
                    g.rear_serial_loop('COM13', 115200)
                self.assertTrue(stream.opened and stream.closed)
                self.assertIsNone(g.serial_stream)
                g.serial_stream = stream  # Defense in depth, even on accidental registration.
                self.assertEqual(g._send_vehicle_command({'type': 'live_tv'}), '')
                self.assertTrue(g.snapshot()['usb_link_online'])
                self.assertEqual(g.snapshot()['input_mode'], 'STM_USB')
                self.assertIsNone(project(g.snapshot())['rpm_right'])
                g.set_recording(False)
                with closing(sqlite3.connect(path)) as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT payload_json FROM telemetry_samples')]
                    received = [p for p in rows if p.get('stm_raw_line')]
                    self.assertGreater(len(received), 0)
                    self.assertEqual(received[0]['stm_fields']['future'], '42')
                    self.assertEqual(received[0]['power_right_kw'], 3.5)
                    self.assertEqual(received[0]['rpm_right'], 7980)
                    self.assertEqual(received[0]['rpm_right_reported'], 7980)
                history = g.database.tuning_samples(1)
                saved = [r for r in history['samples'] if r['values']['rpm_right_reported'] is not None]
                self.assertEqual(saved[0]['values']['rpm_right_reported'], 7980)
                self.assertEqual(saved[0]['status']['rpm_right_quality'], 'NOISY')
            finally:
                g.database.close()
                g.forward_socket.close()

    def test_stale_usb_is_not_reported_as_connected(self):
        store = TelemetryStore()
        store.update(RearStatusParser().parse(LINE), now=10)
        self.assertTrue(store.snapshot(now=10)['stm_online'])
        self.assertFalse(store.snapshot(now=12)['stm_online'])
        self.assertTrue(all(v is None for v in project(store.snapshot(now=12)).values()))
        self.assertEqual(rpm_status(store.snapshot(now=12))['rpm_right_quality'], 'NO_DATA')


if __name__ == '__main__':
    unittest.main()
