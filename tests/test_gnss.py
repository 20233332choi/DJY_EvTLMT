import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'gateway'))
from gnss import GNSSManager, ll
from ev_gateway import EVGateway, make_handler, TelemetryDatabase

ORIGIN = [35.14358, 126.93324]


def point(x, y, ts, **extra):
    lat, lon = ll(x,y,ORIGIN)
    return dict(lat=lat,lon=lon,ts=ts,accuracy_m=1,speed_kmh=40,heading=0,
                source_id='phone-a',stream_id='stream-a',**extra)


def feed(manager, x, y, ts, delay=.1, **changes):
    p=point(x,y,ts)
    p.update(changes)
    return manager.ingest(p,now=ts+delay)


def setup(manager):
    manager.command(dict(action='gate',a=ll(-10,0,ORIGIN),b=ll(10,0,ORIGIN),min_lap_s=5,min_distance_m=20))
    feed(manager,0,-20,1000)
    manager.command({'action':'start'},now=1000.1)


LOOP = [(0,-10),(0,10),(0,20),(10,20),(20,20),(20,10),(20,0),
        (20,-10),(20,-20),(10,-20),(0,-20),(0,-10),(0,10)]


class GNSSTests(unittest.TestCase):
    def test_short_lap_ignores_legacy_minimum_settings(self):
        m=GNSSManager();setup(m)
        m.gate.update(min_lap_s=3600,min_distance_m=10000)
        for i,y in enumerate([-6,1,-6,1]):feed(m,0,y,1001+i)
        self.assertEqual(len(m.laps),1)
        self.assertAlmostEqual(m.laps[0]['lap_time_s'],2)
        self.assertAlmostEqual(m.laps[0]['distance_m'],14,places=5)

    def test_direction_and_source_time_interpolation(self):
        m=GNSSManager();setup(m)
        for i,(x,y) in enumerate(LOOP):
            feed(m,x,y,1001+i,delay=(i%3)*.7)
        self.assertEqual(len(m.laps),1)
        self.assertAlmostEqual(m.laps[0]['lap_time_s'],11,places=6)
        self.assertGreater(m.laps[0]['estimated_error_s'],0)
        self.assertEqual(m.status(1013.1)['lap_count'],1)
        # Crossing backward never starts a fresh run.
        m.command({'action':'stop'});feed(m,0,10,1020)
        m.command({'action':'start'},now=1020.1)
        feed(m,0,10,1021);feed(m,0,-10,1022)
        self.assertIsNone(m.anchor)

    def test_finite_gate_jitter_and_timestamp_guards(self):
        m=GNSSManager();setup(m)
        feed(m,30,-10,1001);feed(m,30,10,1002)
        self.assertIsNone(m.anchor)
        for i in range(20):
            feed(m,0,(-1)**i*.5,1003+i,speed_kmh=0)
        self.assertEqual(len(m.laps),0)
        self.assertIsNone(m.anchor)
        self.assertFalse(feed(m,0,0,1022)['accepted'])
        self.assertFalse(m.ingest(point(0,0,2000),now=1030)['accepted'])
        self.assertFalse(feed(m,0,0,1023,source_id='phone-b')['accepted'])

    def test_gap_jump_and_missing_source_time_cannot_complete_lap(self):
        for scenario in ('gap','jump','accuracy','delay','stream'):
            with self.subTest(scenario=scenario):
                m=GNSSManager();setup(m)
                feed(m,0,-10,1001);feed(m,0,10,1002)
                self.assertIsNotNone(m.anchor)
                if scenario=='gap':feed(m,0,20,1006)
                if scenario=='jump':self.assertFalse(feed(m,1000,1000,1003)['accepted'])
                if scenario=='accuracy':feed(m,0,20,1003,accuracy_m=15)
                if scenario=='delay':feed(m,0,20,1003,delay=4)
                if scenario=='stream':feed(m,0,20,1003,stream_id='restarted')
                self.assertIsNone(m.anchor)
                self.assertFalse(m.laps)
        m=GNSSManager();p=point(0,0,1000);del p['ts']
        self.assertTrue(m.ingest(p,now=1000)['accepted'])
        self.assertFalse(m.latest['timing_ok'])
        with self.assertRaises(ValueError):
            m.command({'action':'start'},now=1000)

    def test_calibration_preserves_raw_and_accuracy_and_expires(self):
        m=GNSSManager();m.command({'action':'collect'},now=1000)
        for i in range(32):feed(m,(-1)**i*.2,0,1000+i,speed_kmh=0)
        report=m.calibration_report(1031.1)
        self.assertTrue(report['ready'])
        ref=ll(5,10,ORIGIN)
        m.command(dict(action='calibrate',lat=ref[0],lon=ref[1],reference_type='surveyed'),now=1031.1)
        p=feed(m,0,0,1032)['fix']
        self.assertAlmostEqual(p['lat'],ref[0],places=6)
        self.assertAlmostEqual(p['lon'],ref[1],places=6)
        self.assertEqual(p['raw_lat'],ORIGIN[0])
        self.assertEqual(p['accuracy_m'],1)
        p=feed(m,0,0,1033,source_id='phone-b')['fix']
        self.assertIsNone(p['calibration_id'])
        p=feed(m,0,0,100000)['fix']
        self.assertIsNone(p['calibration_id'])
        m.command({'action':'collect'},now=110000)
        for i in range(32):feed(m,i,0,110000+i,speed_kmh=10)
        self.assertFalse(m.calibration_report(110032)['ready'])

    def test_archive_settings_laps_raw_history_and_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'test.db';db=TelemetryDatabase(path)
            try:
                m=GNSSManager(db.gnss);setup(m)
                for i,(x,y) in enumerate(LOOP):feed(m,x,y,1001+i)
                h=db.gnss.history(m.run_id)
                self.assertEqual(len(h['points']),len(LOOP))
                self.assertEqual(len(h['laps']),1)
                self.assertIn('raw_lat',h['points'][0])
                self.assertEqual(h['settings']['gate'],m.gate)
                self.assertFalse(h['more'])
            finally:db.close()
            db=TelemetryDatabase(path)
            try:
                m=GNSSManager(db.gnss)
                self.assertFalse(m.active)
                self.assertIsNotNone(m.gate)
                self.assertEqual(db.gnss.runs()[0]['status'],'INTERRUPTED')
            finally:db.close()

    def test_http_public_map_readonly_and_sample_batch(self):
        g=EVGateway('127.0.0.1',0,'127.0.0.1',19004)
        server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(g))
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}'
        try:
            for path in ['/map','/map/app.js','/map/map.css','/vendor/leaflet/leaflet.js','/phone/app.js']:
                with urllib.request.urlopen(base+path,timeout=2) as r:self.assertEqual(r.status,200)
            req=urllib.request.Request(base+'/api/gnss/live',headers={'X-Forwarded-For':'203.0.113.5'})
            with urllib.request.urlopen(req,timeout=2) as r:
                status=json.load(r)
                self.assertFalse(status['can_edit'])
                self.assertTrue(status['can_time_edit'])
            req=urllib.request.Request(base+'/api/gnss/control',data=b'{"action":"collect"}',headers={'X-Forwarded-For':'203.0.113.5'})
            with self.assertRaises(urllib.error.HTTPError) as e:urllib.request.urlopen(req,timeout=2)
            self.assertEqual(e.exception.code,403)
            import time
            now=time.time()
            req=urllib.request.Request(base+'/api/gnss',data=json.dumps({'samples':[point(0,0,now-.5),point(0,1,now)]}).encode())
            with urllib.request.urlopen(req,timeout=2) as r:self.assertEqual(len(json.load(r)['results']),2)
            self.assertEqual(len(g.gnss.read()['points']),2)
            self.assertIn('gnss_source_timestamp_s',g.snapshot())
            public_headers={'X-Forwarded-For':'203.0.113.5','Content-Type':'application/json','Origin':base}
            for payload in [dict(action='gate',a=ll(-10,0,ORIGIN),b=ll(10,0,ORIGIN)),{'action':'start'},{'action':'stop'}]:
                req=urllib.request.Request(base+'/api/gnss/control',data=json.dumps(payload).encode(),headers=public_headers)
                with urllib.request.urlopen(req,timeout=2) as r:self.assertTrue(json.load(r)['ok'])
            self.assertNotIn('min_lap_s',g.gnss.gate)
            for path,payload,headers,expected in [
                ('/api/control',{},public_headers,403),
                ('/api/gnss/control',{'action':'gate','a':ll(-10,0,ORIGIN),'b':ll(10,0,ORIGIN)},dict(public_headers,Origin='https://other.example'),400)]:
                req=urllib.request.Request(base+path,data=json.dumps(payload).encode(),headers=headers)
                with self.assertRaises(urllib.error.HTTPError) as e:urllib.request.urlopen(req,timeout=2)
                self.assertEqual(e.exception.code,expected)
        finally:
            server.shutdown();server.server_close();g.forward_socket.close()

    def test_history_pages_freeze_endpoint_and_null_speed_calibration(self):
        m=GNSSManager();m.command({'action':'collect'},now=1000)
        for i in range(32):feed(m,0,0,1000+i,speed_kmh=None)
        self.assertTrue(m.calibration_report(1031.1)['ready'])
        with tempfile.TemporaryDirectory() as folder:
            db=TelemetryDatabase(Path(folder)/'pages.db')
            try:
                run=db.gnss.start('pages',{})
                for p in range(1005):db.gnss.record(run,{'ts':p})
                first=db.gnss.history(run)
                self.assertEqual(len(first['points']),1000)
                self.assertTrue(first['more'])
                db.gnss.record(run,{'ts':1005})
                second=db.gnss.history(run,first['cursor'],first['through'])
                self.assertEqual(len(second['points']),5)
                self.assertFalse(second['more'])
                self.assertEqual(second['points'][-1]['ts'],1004)
            finally:db.close()


if __name__=='__main__':unittest.main()
