"""Shared recording controls and allowlisted archive views; no vehicle commands."""
import csv
import io
import json
import math
import time
from datetime import datetime
from http import HTTPStatus
from urllib.parse import parse_qs

from battery import CHANNELS as BATTERY_CHANNELS, project as battery_project, status
from tuning import CHANNELS as TUNING_CHANNELS, project as tuning_project, rpm_status

EXTRA_CHANNELS = (
    ('power_left_w', '좌측 출력 요구 · 실측 아님', 'W', 'power'),
    ('power_right_w', '우측 출력 요구 · 실측 아님', 'W', 'power'),
    ('power_command_total_w', '좌우 출력 요구 합계 · 실측 아님', 'W', 'power'),
    ('delta_power_w', '좌우 차동 출력 요구 ΔP', 'W', 'power'),
    ('gnss_latitude_deg', '위도', '°', 'gps'),
    ('gnss_longitude_deg', '경도', '°', 'gps'),
    ('gnss_accuracy_m', 'GPS 정확도', 'm', 'gps'),
    ('gnss_speed_kmh', 'GPS 속도', 'km/h', 'gps'),
    ('gnss_heading_deg', 'GPS 진행 방향', '°', 'gps'),
    ('gnss_altitude_m', 'GPS 고도', 'm', 'gps'),
    ('gnss_age_ms', 'GPS 표본 나이', 'ms', 'gps'),
) + tuple((f'cell_{i}_v', f'셀 {i} 전압', 'V', 'cells') for i in range(1, 49))
CHANNELS = tuple({c[0]: c for c in (*TUNING_CHANNELS, *BATTERY_CHANNELS, *EXTRA_CHANNELS)}.values())


def channel_category(key):
    if key.startswith('cell_'):
        return 'cells'
    if key.startswith(('battery_', 'bms_')):
        return 'battery'
    if key.startswith('gnss_'):
        return 'gps'
    if key.startswith(('power_', 'delta_power')):
        return 'power'
    if key.startswith(('sas_', 'tps_')):
        return 'inputs'
    if key.startswith(('rpm_', 'motor_', 'speed_', 'vehicle_speed')):
        return 'drive'
    return 'tv'


def epoch(text):
    return datetime.fromisoformat(text.replace('Z', '+00:00')).timestamp()


def public_recording_status(gateway):
    state = gateway.database.status() if gateway.database else {}
    result = {k: v for k, v in state.items() if k.startswith('recording_')}
    result['can_record'] = gateway.database is not None
    result['now_ms'] = time.time_ns() // 1000000
    return result


def archive_page(db, sid, after=0, through=0, limit=1000):
    with db._lock:
        session = db._connection.execute(
            'SELECT id,started_at_utc,stopped_at_utc,label,sample_count FROM measurement_sessions WHERE id=?', (sid,)
        ).fetchone()
        if not session:
            raise ValueError('기록을 찾을 수 없습니다.')
        if not through:
            through = db._connection.execute('SELECT COALESCE(MAX(id),0) FROM telemetry_samples WHERE session_id=?', (sid,)).fetchone()[0]
        rows = db._connection.execute(
            'SELECT id,captured_at_utc,source_timestamp_ms,payload_json,received_at_utc FROM telemetry_samples '
            'WHERE session_id=? AND id>? AND id<=? ORDER BY id LIMIT ?',
            (sid, after, through, max(1, min(1000, limit)))
        ).fetchall()
    start = epoch(session[1])
    samples = []
    for row in rows:
        packet = json.loads(row[3])
        values = {**tuning_project(packet), **battery_project(packet)}
        for key in ('power_left', 'power_right', 'power_command_total', 'delta_power'):
            v = values.get(key+'_kw')
            values[key+'_w'] = v*1000 if isinstance(v, (int, float)) and math.isfinite(v) else None
        for key, *_ in EXTRA_CHANNELS:
            if key.startswith('gnss_'):
                v = packet.get(key)
                values[key] = v if packet.get('gnss_online') and isinstance(v, (int, float)) and math.isfinite(v) else None
        bms = status(packet)
        for i, value in enumerate(bms.get('bms_cell_voltages_v', []), 1):
            values[f'cell_{i}_v'] = value
        samples.append({'id': row[0], 'captured_at_utc': row[1], 'received_at_utc': row[4],
                        'source_timestamp_ms': row[2],
                        'elapsed_s': round(epoch(row[4] or row[1]) - start, 3),
                        'sample_elapsed_s': round(epoch(row[1]) - start, 3),
                        'gnss_source_timestamp_s': packet.get('gnss_source_timestamp_s') if packet.get('gnss_online') else None,
                        'values': values, 'status': {**rpm_status(packet), **bms}})
    return {'session': dict(zip(('id', 'started_at_utc', 'stopped_at_utc', 'label', 'sample_count'), session)),
            'samples': samples, 'through': through, 'cursor': samples[-1]['id'] if samples else after,
            'more': bool(samples and samples[-1]['id'] < through),
            'channels': [{'key': c[0], 'label': c[1], 'unit': c[2], 'category': channel_category(c[0])} for c in CHANNELS]}


def serve_recording_get(handler, gateway, parsed, root):
    path = parsed.path
    assets = {'/records': 'analysis.html', '/records/': 'analysis.html', '/analysis': 'analysis.html', '/analysis/': 'analysis.html',
              '/records/analysis.js': 'analysis.js', '/records/analysis.css': 'analysis.css',
              '/records/clock.js': 'clock.js'}
    if path in assets:
        name = assets[path]
        mime = 'text/javascript' if name.endswith('.js') else 'text/css' if name.endswith('.css') else 'text/html'
        handler._send((root / 'web/records' / name).read_bytes(), mime + '; charset=utf-8')
        return True
    if not path.startswith('/api/records/'):
        return False
    try:
        if path == '/api/records/status':
            result = public_recording_status(gateway)
        elif not gateway.database:
            raise ValueError('기록 DB가 비활성화되어 있습니다.')
        elif path == '/api/records/sessions':
            result = {'sessions': gateway.database.list_sessions(500)}
        elif path in {'/api/records/samples', '/api/records/export.csv'}:
            query = parse_qs(parsed.query)
            sid, after, through = (int(query.get(k, ['0'])[0]) for k in ('session_id', 'after', 'through'))
            if sid <= 0 or min(after, through) < 0:
                raise ValueError('잘못된 기록 번호 또는 페이지입니다.')
            result = archive_page(gateway.database, sid, after, through)
            if path.endswith('.csv'):
                # Freeze the upper cursor and stream bounded pages, including all rows.
                handler.send_response(200)
                handler.send_header('Content-Type', 'text/csv; charset=utf-8')
                handler.send_header('Content-Disposition', f'attachment; filename="ev-session-{sid}.csv"')
                handler.send_header('Cache-Control', 'no-store')
                handler.send_header('Connection', 'close')
                handler.end_headers()
                handler.close_connection = True
                keys = [c[0] for c in CHANNELS]
                buffer = io.StringIO()
                writer = csv.writer(buffer)
                writer.writerow(['id', 'captured_at_utc', 'received_at_utc', 'elapsed_s', 'sample_elapsed_s', 'source_timestamp_ms', 'gnss_source_timestamp_s', *keys, 'status_json'])
                handler.wfile.write(b'\xef\xbb\xbf')
                while True:
                    for sample in result['samples']:
                        writer.writerow([sample[k] for k in ('id', 'captured_at_utc', 'received_at_utc', 'elapsed_s', 'sample_elapsed_s', 'source_timestamp_ms', 'gnss_source_timestamp_s')] +
                                        [sample['values'].get(k) for k in keys] + [json.dumps(sample['status'], ensure_ascii=False)])
                    handler.wfile.write(buffer.getvalue().encode('utf-8'))
                    buffer.seek(0)
                    buffer.truncate(0)
                    if not result['more']:
                        break
                    result = archive_page(gateway.database, sid, result['cursor'], result['through'])
                return True
        else:
            handler._send(b'not found', 'text/plain', HTTPStatus.NOT_FOUND)
            return True
        handler._send(json.dumps(result, ensure_ascii=False, allow_nan=False).encode('utf-8'), 'application/json; charset=utf-8')
    except (ValueError, TypeError) as exc:
        handler._send(json.dumps({'error': str(exc)}, ensure_ascii=False).encode('utf-8'), 'application/json; charset=utf-8', HTTPStatus.BAD_REQUEST)
    return True
