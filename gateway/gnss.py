"""WGS84 track, stationary reference alignment and source-time lap timing.

UI/workflow reference: DJY-BAJA-_TLMTSYS, commit 3ecaa42.
Timing uses unsmoothed positions; raw fixes and rejection reasons stay available.
"""
from collections import deque
import json
import math
import statistics
import threading
import time
import uuid

EARTH = 6371000.0


def number(value, name, low=-math.inf, high=math.inf):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"invalid {name}")
    return float(value)


def xy(lat, lon, origin):
    return (math.radians(lon-origin[1])*EARTH*math.cos(math.radians(origin[0])),
            math.radians(lat-origin[0])*EARTH)


def ll(east, north, origin):
    return [origin[0]+math.degrees(north/EARTH),
            origin[1]+math.degrees(east/(EARTH*math.cos(math.radians(origin[0]))))]


def distance(a, b):
    return math.hypot(*xy(b['lat'], b['lon'], [a['lat'], a['lon']]))


class GNSSArchive:
    """Shares the telemetry database connection and its lock."""
    def __init__(self, connection, lock):
        self.db, self.lock = connection, lock
        with lock:
            connection.executescript('''
                CREATE TABLE IF NOT EXISTS gnss_settings (id INTEGER PRIMARY KEY, payload TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS gnss_runs (id INTEGER PRIMARY KEY AUTOINCREMENT,
                    started REAL, stopped REAL, label TEXT, settings TEXT, status TEXT);
                CREATE TABLE IF NOT EXISTS gnss_fixes (id INTEGER PRIMARY KEY AUTOINCREMENT,
                    run_id INTEGER NOT NULL REFERENCES gnss_runs(id), payload TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS gnss_fixes_run ON gnss_fixes(run_id,id);
                CREATE TABLE IF NOT EXISTS gnss_laps (id INTEGER PRIMARY KEY AUTOINCREMENT,
                    run_id INTEGER NOT NULL REFERENCES gnss_runs(id), payload TEXT NOT NULL);
            ''')
            connection.execute("UPDATE gnss_runs SET stopped=?,status='INTERRUPTED' WHERE stopped IS NULL", (time.time(),))
            connection.commit()

    def settings(self, value=None):
        with self.lock:
            if value is not None:
                self.db.execute('INSERT OR REPLACE INTO gnss_settings VALUES(1,?)', (json.dumps(value),))
                self.db.commit()
            row = self.db.execute('SELECT payload FROM gnss_settings WHERE id=1').fetchone()
            return json.loads(row[0]) if row else {}

    def start(self, label, settings):
        with self.lock:
            cur = self.db.execute("INSERT INTO gnss_runs(started,label,settings,status) VALUES(?,?,?,'RUNNING')",
                                  (time.time(), label, json.dumps(settings)))
            self.db.commit()
            return cur.lastrowid

    def stop(self, run):
        with self.lock:
            self.db.execute("UPDATE gnss_runs SET stopped=?,status='STOPPED' WHERE id=?", (time.time(), run))
            self.db.commit()

    def record(self, run, fix, lap=None):
        with self.lock:
            self.db.execute('INSERT INTO gnss_fixes(run_id,payload) VALUES(?,?)', (run, json.dumps(fix)))
            if lap:
                self.db.execute('INSERT INTO gnss_laps(run_id,payload) VALUES(?,?)', (run, json.dumps(lap)))
            self.db.commit()

    def runs(self):
        with self.lock:
            return [dict(zip(('id','started','stopped','label','status'), r)) for r in
                    self.db.execute('SELECT id,started,stopped,label,status FROM gnss_runs ORDER BY id DESC LIMIT 200')]

    def history(self, run, after=0, through=0):
        with self.lock:
            setting = self.db.execute('SELECT settings FROM gnss_runs WHERE id=?', (run,)).fetchone()
            if setting is None:
                raise ValueError('unknown GNSS run')
            if not through:
                through = self.db.execute('SELECT COALESCE(MAX(id),0) FROM gnss_fixes WHERE run_id=?', (run,)).fetchone()[0]
            rows = self.db.execute('SELECT id,payload FROM gnss_fixes WHERE run_id=? AND id>? AND id<=? ORDER BY id LIMIT 1001',
                                   (run, after, through)).fetchall()
            laps = [json.loads(r[0]) for r in self.db.execute('SELECT payload FROM gnss_laps WHERE run_id=? ORDER BY id', (run,))]
            points = [dict(json.loads(row[1]), id=row[0]) for row in rows[:1000]]
            return dict(points=points, laps=laps, settings=json.loads(setting[0]), through=through,
                        more=len(rows)>1000, cursor=points[-1]['id'] if points else after)


class GNSSManager:
    def __init__(self, archive=None):
        self.lock = threading.RLock()
        self.archive = archive
        saved = archive.settings() if archive else {}
        self.gate = saved.get('gate')
        self.calibration = saved.get('calibration')
        self.epoch = uuid.uuid4().hex
        self.points = deque(maxlen=12000)
        self.recent = deque(maxlen=2000)
        self.latest = self.previous = None
        self.serial = self.segment = 0
        self.last_ts = {}
        self.collect_since = None
        self.active = False
        self.run_id = 0
        self.run_source = None
        self.laps = []
        self.anchor = None
        self.anchor_error = 0
        self.path_m = 0
        self.armed = False
        self.reason = 'WAITING_GPS'
        self.rejected = 0

    def _settings(self):
        return dict(gate=self.gate, calibration=self.calibration)

    def _save(self):
        if self.archive:
            self.archive.settings(self._settings())

    def _break(self):
        self.previous = None
        self.anchor = None
        self.path_m = 0
        self.armed = False
        self.segment += 1

    def ingest(self, data, now=None):
        now = time.time() if now is None else now
        lat = number(data.get('lat', data.get('latitude')), 'latitude', -85, 85)
        lon = number(data.get('lon', data.get('longitude')), 'longitude', -180, 180)
        acc = number(data.get('accuracy_m', data.get('accuracy')), 'accuracy', 0.01, 100)
        supplied_time = data.get('ts') is not None
        ts = number(data['ts'] if supplied_time else now, 'GPS timestamp', 1)
        speed = None if data.get('speed_kmh') is None else number(data['speed_kmh'], 'GPS speed', 0, 300)
        heading = None if data.get('heading') is None else number(data['heading'], 'heading', 0, 360) % 360
        altitude = None if data.get('altitude') is None else number(data['altitude'], 'altitude', -1000, 20000)
        source = str(data.get('source_id', 'legacy-phone'))[:64]
        stream = str(data.get('stream_id', source))[:64]
        with self.lock:
            reason = ''
            if self.active and self.run_source and self.run_source != source:
                reason = 'OTHER_SOURCE'
            elif ts <= self.last_ts.get(source, 0):
                reason = 'OUT_OF_ORDER'
            elif ts > now+5 or now-ts > 30:
                reason = 'CLOCK_OR_OLD_FIX'
            if reason:
                self.rejected += 1
                return dict(accepted=False, reason=reason)
            self.last_ts[source] = ts
            if len(self.last_ts) > 100:
                self.last_ts = {source: ts}
            self.serial += 1
            fix = dict(id=self.serial, ts=ts, received_ts=now, timestamp_source='GPS' if supplied_time else 'RECEIVE',
                       raw_lat=lat, raw_lon=lon, lat=lat, lon=lon, accuracy_m=acc,
                       speed_kmh=speed, heading=heading, altitude=altitude, source_id=source, stream_id=stream,
                       calibration_id=None, accepted=True, timing_ok=False, reason='', segment=self.segment)
            cal = self.calibration
            if cal and cal['source_id'] == source and now-cal['created'] <= 86400:
                fix['lat'], fix['lon'] = ll(cal['east_m'], cal['north_m'], [lat, lon])
                fix['calibration_id'] = cal['id']
            old = self.previous
            if old and (old['source_id'] != source or old['stream_id'] != stream or old['calibration_id'] != fix['calibration_id']):
                self._break()
                old = None
            dt = ts-old['ts'] if old else None
            step = distance(old, fix) if old else 0
            if acc > 20:
                reason = 'LOW_ACCURACY'
            elif old and dt <= 3 and step > 60*dt + old['accuracy_m']+acc:
                reason = 'POSITION_JUMP'
            fix['accepted'] = not bool(reason)
            timing = not reason and supplied_time and now-ts <= 3 and acc <= 10
            if not timing:
                self._break()
                old = None
                reason = reason or ('NO_SOURCE_TIME' if not supplied_time else 'DELAYED_FIX' if now-ts>3 else 'TIMING_ACCURACY')
            elif dt is not None and dt > 3:
                self._break()
                old = None
                reason = 'SAMPLE_GAP'
            fix['timing_ok'] = timing
            fix['reason'] = reason
            fix['segment'] = self.segment
            if not fix['accepted']:
                self.rejected += 1
            lap = None
            if self.active and timing:
                self.run_source = self.run_source or source
                lap = self._cross(old, fix)
            if timing:
                self.previous = fix
            self.latest = fix
            self.points.append(fix)
            self.recent.append(fix)
            self.reason = reason or ('TIMING' if self.anchor is not None else 'WAITING_CROSSING' if self.active else 'READY')
            if self.active and self.archive:
                self.archive.record(self.run_id, fix, lap)
            return dict(accepted=fix['accepted'], reason=self.reason, fix=dict(fix))

    def _gate_xy(self, fix):
        a, b = self.gate['a'], self.gate['b']
        bx, by = xy(*b, a)
        px, py = xy(fix['lat'], fix['lon'], a)
        length = math.hypot(bx, by)
        return (bx*py-by*px)/length, (bx*px+by*py)/length, length

    def _cross(self, old, fix):
        d, along, length = self._gate_xy(fix)
        if d <= -max(5, fix['accuracy_m']):
            self.armed = True
        if old is None:
            return None
        step = distance(old, fix)
        self.path_m += step
        od, oa, _ = self._gate_xy(old)
        if not (self.armed and od < 0 <= d):
            return None
        self.armed = False
        fraction = -od/(d-od)
        crossing_along = oa+(along-oa)*fraction
        normal_speed = (d-od)/(fix['ts']-old['ts'])
        # Endpoints are finite; GPS motion must carry the car across the line.
        if not 0 <= crossing_along <= length or normal_speed < 1.5 or (fix['speed_kmh'] is not None and fix['speed_kmh'] < 3):
            return None
        stamp = old['ts']+(fix['ts']-old['ts'])*fraction
        estimate = max(old['accuracy_m'], fix['accuracy_m'])/normal_speed + (fix['ts']-old['ts'])/2
        lap = None
        if self.anchor is not None:
            elapsed = stamp-self.anchor
            lap = dict(number=len(self.laps)+1, start_ts=self.anchor, end_ts=stamp, lap_time_s=elapsed,
                       distance_m=self.path_m, estimated_error_s=estimate+self.anchor_error,
                       calibration_id=fix['calibration_id'])
            self.laps.append(lap)
        self.anchor, self.anchor_error = stamp, estimate
        self.path_m = 0
        return lap

    def calibration_report(self, now=None):
        now = time.time() if now is None else now
        if self.collect_since is None:
            return dict(collecting=False, ready=False, count=0, elapsed_s=0)
        rows = [p for p in self.recent if p['received_ts'] >= self.collect_since and p['received_ts'] <= self.collect_since+45]
        if not rows:
            return dict(collecting=True, ready=False, count=0, elapsed_s=0)
        good = [p for p in rows if p['accepted'] and p['timing_ok'] and p['accuracy_m'] <= 10]
        if not good:
            return dict(collecting=True, ready=False, count=0, elapsed_s=0)
        center = [statistics.median(p['raw_lat'] for p in good), statistics.median(p['raw_lon'] for p in good)]
        radii = sorted(math.hypot(*xy(p['raw_lat'], p['raw_lon'], center)) for p in good)
        spread = radii[min(len(radii)-1, math.ceil(len(radii)*.95)-1)]
        duration = good[-1]['ts']-good[0]['ts']
        gaps = any(b['ts']-a['ts'] > 3 for a,b in zip(good, good[1:]))
        first = [statistics.median(p[k] for p in good[:5]) for k in ('raw_lat','raw_lon')]
        last = [statistics.median(p[k] for p in good[-5:]) for k in ('raw_lat','raw_lon')]
        drift = math.hypot(*xy(*last, first))
        stationary = all(p['speed_kmh'] is None or p['speed_kmh'] <= 2 for p in good) and drift <= 2
        one_source = len({(p['source_id'],p['stream_id']) for p in rows}) == 1
        ready = len(good)>=20 and duration>=30 and spread<=5 and stationary and one_source and not gaps and len(good)==len(rows)
        return dict(collecting=True, ready=ready, count=len(good), elapsed_s=duration, spread95_m=spread,
                    center=center, stationary=stationary, drift_m=drift, source_id=good[-1]['source_id'],
                    recent=now-good[-1]['received_ts']<=15)

    def command(self, data, now=None):
        now = time.time() if now is None else now
        action = data.get('action')
        with self.lock:
            if action == 'stop':
                if self.active and self.archive:
                    self.archive.stop(self.run_id)
                self.active = False
                self._break()
            elif self.active:
                raise ValueError('stop the lap run before changing its settings')
            elif action == 'gate':
                points = []
                for key in ('a','b'):
                    point = data.get(key)
                    if not isinstance(point, list) or len(point)!=2:
                        raise ValueError('two gate endpoints required')
                    points.append([number(point[0], 'latitude', -85,85), number(point[1], 'longitude',-180,180)])
                if not 5 <= math.hypot(*xy(*points[1],points[0])) <= 200:
                    raise ValueError('start line must be 5 to 200 m wide')
                self.gate = dict(a=points[0], b=points[1])
                self._break()
                self._save()
            elif action == 'collect':
                self.collect_since = now
            elif action == 'calibrate':
                report = self.calibration_report(now)
                if not report.get('ready') or not report.get('recent'):
                    raise ValueError('need 30 s / 20 stable stationary fixes, accuracy <=10 m, spread <=5 m')
                ref = [number(data.get('lat'),'reference latitude',-85,85), number(data.get('lon'),'reference longitude',-180,180)]
                east, north = xy(*ref,report['center'])
                if math.hypot(east,north)>100:
                    raise ValueError('reference offset exceeds 100 m; check the reference')
                self.calibration = dict(id=uuid.uuid4().hex[:12], source_id=report['source_id'], created=now,
                                        east_m=east, north_m=north, reference=ref, measured=report['center'],
                                        spread95_m=report['spread95_m'], count=report['count'],
                                        reference_type='surveyed' if data.get('reference_type')=='surveyed' else 'map')
                self.collect_since = None
                self._break()
                self._save()
            elif action == 'clear_calibration':
                self.calibration = None
                self._break()
                self._save()
            elif action == 'start':
                if not self.gate:
                    raise ValueError('set the start line first')
                if not self.latest or not self.latest['timing_ok'] or now-self.latest['received_ts']>3 or now-self.latest['ts']>3:
                    raise ValueError('fresh GPS timestamp and accuracy <=10 m required')
                self._break()
                self.laps = []
                self.run_source = self.latest['source_id']
                self.run_id = self.archive.start(str(data.get('label',''))[:80],self._settings()) if self.archive else self.run_id+1
                self.active = True
            else:
                raise ValueError('unknown GNSS action')
            return self.status(now)

    def status(self, now=None):
        now = time.time() if now is None else now
        with self.lock:
            latest = dict(self.latest) if self.latest else None
            live = bool(latest and latest['accepted'] and now-latest['received_ts']<=3 and now-latest['ts']<=3)
            current = latest['ts']-self.anchor if live and self.active and self.anchor is not None else None
            return dict(epoch=self.epoch, latest=latest, live=live, active=self.active, run_id=self.run_id,
                        reason=self.reason if live else 'GPS_STALE', rejected=self.rejected,
                        gate=self.gate, calibration=self.calibration, calibration_report=self.calibration_report(now),
                        lap_count=len(self.laps), laps=list(self.laps), lap_time_s=current,
                        last_lap_s=self.laps[-1]['lap_time_s'] if self.laps else None,
                        best_lap_s=min((p['lap_time_s'] for p in self.laps),default=None))

    def read(self, after=0):
        with self.lock:
            rows = [p for p in self.points if p['id']>after]
            return dict(points=rows[:1000], cursor=rows[min(len(rows),1000)-1]['id'] if rows else after,
                        more=len(rows)>1000, truncated=bool(self.points and after and after<self.points[0]['id']-1),
                        **self.status())

    def telemetry(self):
        status = self.status()
        p = status['latest']
        if p is None:
            return {}
        return dict(gnss_online=status['live'], gnss_latitude_deg=p['lat'], gnss_longitude_deg=p['lon'],
                    gnss_raw_latitude_deg=p['raw_lat'], gnss_raw_longitude_deg=p['raw_lon'],
                    gnss_accuracy_m=p['accuracy_m'], gnss_heading_deg=p['heading'], gnss_altitude_m=p['altitude'],
                    gnss_speed_kmh=p['speed_kmh'], gnss_source='PHONE GPS', gnss_source_timestamp_s=p['ts'],
                    gnss_received_timestamp_s=p['received_ts'], gnss_calibration_id=p['calibration_id'],
                    gnss_age_ms=max(0,(time.time()-p['ts'])*1000), gnss_fix_type=3 if p['altitude'] is not None else 2,
                    lap_time_s=status['lap_time_s'] or 0, last_lap_s=status['last_lap_s'] or 0,
                    best_lap_s=status['best_lap_s'] or 0, gnss_lap_active=self.active, gnss_lap_count=status['lap_count'])
