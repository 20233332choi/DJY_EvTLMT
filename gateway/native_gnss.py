"""Traccar Client (iOS/Android) OsmAnd HTTP input for the existing GNSS pipeline.

Native location execution belongs to the phone app, never a service worker.
Protocol: https://www.traccar.org/osmand/ (speed in knots).
"""
from datetime import datetime
import hashlib
from http import HTTPStatus
import json
import math
import threading
import time
from urllib.parse import parse_qs, urlparse


def numeric(value, name):
    if isinstance(value, bool):
        raise ValueError(name + ' must be numeric')
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise ValueError(name + ' required') from None
    if not math.isfinite(result):
        raise ValueError(name + ' must be finite')
    return result


def timestamp(value):
    try:
        result = numeric(value, 'timestamp')
        return result / 1000 if result > 100_000_000_000 else result
    except ValueError:
        try:
            parsed = datetime.fromisoformat(str(value).replace('Z', '+00:00'))
            if parsed.tzinfo is None:
                raise ValueError('timestamp needs timezone')
            return parsed.timestamp()
        except (ValueError, OverflowError):
            raise ValueError('valid source timestamp required') from None


def decode(data):
    device = str(data.get('id', data.get('deviceid', ''))).strip()
    if not device or len(device) > 64:
        raise ValueError('device identifier required (1..64 characters)')
    if str(data.get('valid', 'true')).lower() in {'false', '0'}:
        raise ValueError('INVALID_FIX')
    if 'location' in data and 'lat' not in data:
        parts = str(data['location']).split(',')
        if len(parts) == 2:
            data = {**data, 'lat': parts[0], 'lon': parts[1]}
    source = 'traccar-' + hashlib.sha256(device.encode()).hexdigest()[:20]
    result = dict(lat=numeric(data.get('lat'), 'latitude'), lon=numeric(data.get('lon'), 'longitude'),
                  accuracy_m=numeric(data.get('accuracy'), 'accuracy'), ts=timestamp(data.get('timestamp')),
                  source_id=source, stream_id=source)
    for source_key, key in (('speed', 'speed_kmh'), ('bearing', 'heading'), ('altitude', 'altitude')):
        value = data.get(source_key, data.get('heading') if key == 'heading' else None)
        if value is not None and value != '':
            value = numeric(value, source_key)
            # Mobile location APIs use a negative speed/bearing for unavailable values.
            result[key] = None if key in {'speed_kmh', 'heading'} and value < 0 else value * 1.852 if key == 'speed_kmh' else value
    return result


class NativeGNSS:
    def __init__(self):
        self.lock = threading.Lock()
        self.received = self.accepted = self.rejected = 0
        self.last_received = self.last_accepted = self.last_fix = None
        self.reason = 'WAITING_APP'
        self.source = None

    def ingest(self, gateway, data):
        # Serialize native uploads so source order, counters and gateway snapshots agree.
        with self.lock:
            self.received += 1
            self.last_received = time.time()
            try:
                point = decode(data)
                self.source = point['source_id']
                self.last_fix = point['ts']
                result = gateway.update_phone_gnss(point)
            except ValueError as exc:
                result = {'accepted': False, 'reason': str(exc)}
            self.reason = result.get('reason', '')
            if result.get('accepted'):
                self.accepted += 1
                self.last_accepted = self.last_received
            else:
                self.rejected += 1
            # ACK permanent quality/time rejections too; don't trap an old fix at
            # the head of the native app's offline queue. Never relabel it as live.
            return {'ok': True, 'accepted': bool(result.get('accepted')), 'reason': self.reason}

    def status(self):
        with self.lock:
            now = time.time()
            return {'received': self.received, 'accepted': self.accepted, 'rejected': self.rejected,
                    'last_received_ms': None if self.last_received is None else round(self.last_received*1000),
                    'last_fix_ms': None if self.last_fix is None else round(self.last_fix*1000),
                    'receive_age_s': None if self.last_received is None else max(0, now-self.last_received),
                    'live': self.last_accepted is not None and self.last_fix is not None and
                            now-self.last_accepted <= 5 and -5 <= now-self.last_fix <= 5,
                    'reason': self.reason, 'source_id': self.source}


def serve_native_gnss(handler, gateway):
    parsed = urlparse(handler.path)
    if parsed.path == '/api/gnss/native/status' and handler.command == 'GET':
        result = gateway.native_gnss.status()
    elif parsed.path == '/api/gnss/native':
        try:
            params = parse_qs(parsed.query, max_num_fields=64)
            if handler.command == 'POST':
                length = int(handler.headers.get('Content-Length', '0'))
                if not 0 <= length <= 8192:
                    raise ValueError('request body too large')
                raw = handler.rfile.read(length).decode('utf-8')
                if raw:
                    mime = handler.headers.get('Content-Type', '').split(';')[0].strip()
                    if mime != 'application/x-www-form-urlencoded':
                        raise ValueError('OsmAnd form-encoded body required')
                    params.update(parse_qs(raw, max_num_fields=64))
            data = {key: values[-1] for key, values in params.items()}
            if not data:
                raise ValueError('Traccar Client position required')
            result = gateway.native_gnss.ingest(gateway, data)
        except (ValueError, UnicodeDecodeError) as exc:
            handler._send(json.dumps({'ok': False, 'error': str(exc)}).encode(), 'application/json', HTTPStatus.BAD_REQUEST)
            return True
    else:
        return False
    handler._send(json.dumps(result, ensure_ascii=False, allow_nan=False).encode('utf-8'), 'application/json; charset=utf-8')
    return True
