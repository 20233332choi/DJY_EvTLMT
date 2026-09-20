import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from urllib.parse import urlencode
from urllib.request import Request, urlopen
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'gateway'))
from native_gnss import decode, NativeGNSS
from ev_gateway import EVGateway, make_handler


def point(**extra):
    return dict(id='test-iphone', lat='35.14358', lon='126.93324', accuracy='3.5',
                timestamp=str(time.time()), speed='10', bearing='90', **extra)


class NativeGNSSTests(unittest.TestCase):
    def test_osmand_units_source_time_and_missing_values(self):
        raw = point(altitude='20')
        result = decode(raw)
        self.assertAlmostEqual(result['speed_kmh'], 18.52)
        self.assertEqual(result['heading'], 90)
        self.assertEqual(result['ts'], float(raw['timestamp']))
        self.assertEqual(result['source_id'], decode(raw)['source_id'])
        self.assertEqual(decode({**raw, 'timestamp': '1700000000123'})['ts'], 1700000000.123)
        self.assertEqual(decode({**raw, 'timestamp': '2023-11-14T22:13:20Z'})['ts'], 1700000000)
        self.assertIsNone(decode({**raw, 'speed': '-1'})['speed_kmh'])
        for key, value in [('accuracy', None), ('lat', 'nan'), ('timestamp', 'invalid'), ('id', ''), ('valid', 'false')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                decode({**raw, key: value})

    def test_native_http_into_map_and_recording_without_browser(self):
        with tempfile.TemporaryDirectory() as folder:
            gateway = EVGateway('127.0.0.1', 0, '127.0.0.1', 19004, database_path=Path(folder)/'test.db')
            server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(gateway))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            base = f'http://127.0.0.1:{server.server_port}'
            def send(data, get=False):
                encoded = urlencode(data)
                req = Request(base+'/api/gnss/native'+('?' + encoded if get else ''),
                              data=None if get else encoded.encode(),
                              headers={'Content-Type':'application/x-www-form-urlencoded', 'X-Forwarded-For':'203.0.113.9'})
                with urlopen(req, timeout=3) as response:
                    return json.load(response)
            try:
                gateway.set_recording(True, 'native fixture')
                first = point()
                self.assertTrue(send(first)['accepted'])
                self.assertAlmostEqual(gateway.gnss.latest['speed_kmh'], 18.52)
                self.assertEqual(gateway.database.status()['recording_sample_count'], 1)
                saved = gateway.database.list_samples(1)['samples'][0]['telemetry']
                self.assertAlmostEqual(saved['gnss_latitude_deg'], 35.14358)
                duplicate = send(first)
                self.assertTrue(duplicate['ok'])
                self.assertFalse(duplicate['accepted'])
                second = {**point(), 'timestamp': str(float(first['timestamp'])+.1)}
                self.assertTrue(send(second, get=True)['accepted'])
                self.assertEqual(gateway.database.status()['recording_sample_count'], 2)
                delayed = {**point(), 'id':'old-phone', 'timestamp':str(time.time()-60)}
                self.assertEqual(send(delayed)['reason'], 'CLOCK_OR_OLD_FIX')
                self.assertEqual(gateway.database.status()['recording_sample_count'], 2)
                invalid = send({**point(), 'accuracy': '150'})
                self.assertFalse(invalid['accepted'])
                with urlopen(base+'/api/gnss/native/status') as response:
                    status = json.load(response)
                self.assertEqual(status['received'], 5)
                self.assertEqual(status['accepted'], 2)
                self.assertEqual(status['rejected'], 3)
                with urlopen(base+'/phone') as response:
                    self.assertIn('Traccar Client', response.read().decode())
                with self.assertRaises(HTTPError) as invalid:
                    urlopen(Request(base+'/api/gnss/native', data=b'{}', headers={'Content-Type':'application/json'}), timeout=3)
                self.assertEqual(invalid.exception.code, 400)
                invalid.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                gateway.database.close()
                gateway.forward_socket.close()


if __name__ == '__main__':
    unittest.main()
