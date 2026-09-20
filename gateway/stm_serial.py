"""Read-only adapter for Rear's 115200-baud ST-Link debug records.

No board commands, reset or control-logic changes. Display speed uses the
user-confirmed wheel diameter/gear ratio; STM speed remains in its own field.
Raw records remain available even when a measurement cannot be trusted.
"""
import re
import math
import time
from vehicle_geometry import speed_kmh


class RearStatusParser:
    def __init__(self):
        self.sequence = 0
        self.previous = {}

    def parse(self, line):
        try:
            text = line.decode('ascii') if isinstance(line, bytes) else line
            text = text.strip()
            if not text.startswith('L=') or len(text) > 2048:
                return None
            tokens = text.split()
            if not all(re.fullmatch(r'[A-Za-z][\w]*=[+-]?[0-9A-Fa-f]+(?:/[0-9A-Fa-f]+)*', t) for t in tokens):
                return None
            fields = dict(t.split('=', 1) for t in tokens)
            # Known variants may insert diagnostic tuples between glt and tps.
            left, right, tps, pct, idle = (int(fields[k]) for k in ('L', 'R', 'tps', 'pct', 'idle'))
            caps = tuple(map(int, fields['cap'].split('/')))
            glitches = tuple(map(int, fields['glt'].split('/')))
            if not (0 <= left <= 65535 and 0 <= right <= 65535 and
                    0 <= tps <= 4095 and 0 <= idle <= 4095 and 0 <= pct <= 100 and
                    len(caps) == len(glitches) == 2 and
                    all(0 <= v <= 0xffffffff for v in caps + glitches)):
                return None
            integers = {k: int(v) for k, v in fields.items() if re.fullmatch(r'[+-]?\d+', v)}
            rpm_valid = tuple(map(int, fields['rv'].split('/'))) if 'rv' in fields else None
            if rpm_valid is not None and (len(rpm_valid) != 2 or any(v not in (0, 1) for v in rpm_valid)):
                return None
        except (ValueError, KeyError, UnicodeDecodeError):
            return None

        self.sequence += 1
        packet = {
            'seq': self.sequence, 'sequence_source': 'PC_RECEIVE',
            'timestamp_ms': time.monotonic_ns() // 1000000, 'timestamp_source': 'PC_MONOTONIC',
            'stm_raw_line': text, 'stm_fields': fields,
            'stm_online': True, 'uart_ok': True, 'front_source': 'FRONT CAN / REAR USB',
            'rpm_left': left, 'rpm_right': right, 'tps_raw': tps, 'tps_pct': pct,
            'tps_idle_raw': idle, 'speed_online': False,
            'sas_online': False, 'sas_ok': False, 'imu_online': False, 'imu_ok': False,
            'tqv_internal_online': False, 'rear_output_online': False,
            'live_control_allowed': False, 'pit_adjust_allowed': False,
        }
        for side, rpm, cap, glitch in zip(('left', 'right'), (left, right), caps, glitches):
            previous = self.previous.get(side)
            dc, dg = (cap - previous[0], glitch - previous[1]) if previous else (0, 0)
            # Conservative display-quality check, NOT a motor-control filter.
            # No pulses cannot distinguish a stopped motor from a disconnected sensor.
            quality = 'NO_PULSES' if rpm == 0 else 'UNVERIFIED'
            if previous and dc >= 0 and dg >= 0:
                if dc > 0 and dg <= dc and dg * 2 < dc:
                    quality = 'OK'
                elif dc > 0 and dg * 2 >= dc:
                    quality = 'NOISY'
            elif not previous and cap > 0 and glitch * 2 >= cap:
                quality = 'NOISY'
            if rpm_valid is not None and not rpm_valid[0 if side == 'left' else 1]:
                # Firmware qualification overrides a deceptively clean counter
                # delta (e.g. a single edge after timeout). Keep the numeric raw log.
                if quality == 'OK':
                    quality = 'UNVERIFIED'
            self.previous[side] = (cap, glitch)
            packet.update({f'cap_{side}': cap, f'rpm_glitch_{side}': glitch,
                           f'rpm_{side}_reported': rpm, f'rpm_{side}_reported_online': True,
                           f'rpm_{side}_rejected_pct': round(100.0 * dg / dc, 1)
                           if previous and dc > 0 and 0 <= dg <= dc else None,
                           f'rpm_{side}_quality': quality, f'rpm_{side}_online': quality == 'OK',
                           f'motor_{side}_ok': quality == 'OK'})

        # Never invent an RPM setpoint from TPS/power. Current Rear uses power.
        packet['motor_command_mode'] = 'POWER' if all(k in integers for k in ('pl', 'pr')) else 'UNKNOWN'
        for side, key in (('left', 'rpm_target_l'), ('right', 'rpm_target_r')):
            if key in integers and 0 <= integers[key] <= 65535:
                packet[f'rpm_{side}_target'] = integers[key]
                packet[f'rpm_{side}_target_online'] = True
                packet['motor_command_mode'] = 'RPM'

        # fault=1 specifically means Front sensor CAN timeout in this firmware.
        fault = integers.get('fault')
        front_fresh = fault is not None and fault != 1
        packet['front_sensor_online'] = front_fresh
        packet['tps_online'] = front_fresh
        # tv_stm_esp vehicle_params.h: 885..2820 with +/-200 diagnostic margin.
        packet['tps_ok'] = front_fresh and fault != 2 and 685 <= tps <= 3020
        # Rear's masked sas value alone does not prove an installed/valid sensor.
        packet['sas_online'] = packet['sas_ok'] = front_fresh and fault != 5 and integers.get('sas_ok') == 1
        if 'sas' in integers and 0 <= integers['sas'] < 16384:
            packet['sas_raw'] = integers['sas']
        else:
            packet['sas_online'] = packet['sas_ok'] = False

        if 'steer' in fields or 'sv' in fields or 'sc' in fields:
            valid = (integers.get('sv') == 1 and 'sas_raw' in packet and
                     -3142 <= integers.get('steer', 10000) <= 3142 and
                     0 <= integers.get('sc', -1) < 16384)
            packet['sas_online'] = packet['sas_ok'] = front_fresh and fault != 5 and valid
            if valid:
                packet['sas_deg'] = math.degrees(integers['steer'] / 1000.0)
                packet['sas_center_raw'] = integers['sc']

        scaled = {
            'yaw': 'yaw_rate_rad_s', 'lat': 'lateral_accel_m_s2', 'lon': 'longitudinal_accel_m_s2',
            'ax': 'imu_raw_ax_m_s2', 'ay': 'imu_raw_ay_m_s2', 'az': 'imu_raw_az_m_s2',
            'vs': 'vehicle_speed_m_s', 'dy': 'desired_yaw_rad_s', 'ye': 'yaw_error_rad_s',
            'dp': 'delta_power_kw', 'pl': 'power_left_kw', 'pr': 'power_right_kw', 'tr': 'traction_scale',
        }
        for source, target in scaled.items():
            if source in integers and -2147483648 <= integers[source] <= 2147483647:
                packet[target] = integers[source] / 1000.0
        for source, target in {'fault': 'fault_code', 'req': 'tv_requested_pct',
                               'lim': 'tv_limit_pct', 'app': 'tv_applied_pct',
                               'ipk': 'stm_imu_packets', 'bad': 'stm_imu_bad', 'rs': 'stm_imu_resync',
                               'urx': 'rear_command_rx', 'uerr': 'rear_command_errors'}.items():
            if source in integers:
                packet[target] = integers[source]
        packet['tv_active'] = integers.get('tva', integers.get('tv')) == 1
        packet['ed_active'] = integers.get('eda', integers.get('ed')) == 1
        packet['tqv_internal_online'] = (
            all(k in integers for k in ('vs', 'dy', 'ye', 'dp', 'pl', 'pr', 'tr', 'tva', 'eda'))
            and 0 <= integers['tr'] <= 1000 and integers['tva'] in (0, 1)
            and integers['eda'] in (0, 1) and integers['tva'] + integers['eda'] <= 1)
        packet['imu_online'] = packet['imu_ok'] = integers.get('imu') == 1 and 'yaw_rate_rad_s' in packet
        packet['pid_online'] = all(k in integers and 0 <= integers[k] <= 2147483647
                                   for k in ('kp', 'ki', 'kd'))
        if packet['pid_online']:
            packet.update({f'pid_{key}': integers[key] / 1000.0 for key in ('kp', 'ki', 'kd')})
        if re.fullmatch(r'\d+/\d+', fields.get('dac', '')):
            dl, dr = map(int, fields['dac'].split('/'))
            if dl <= 4095 and dr <= 4095:
                packet.update(dac_left=dl, dac_right=dr, rear_output_online=True)
        # Preserve vehicle_speed_m_s as reported by STM. Display speed uses
        # the confirmed 45 cm diameter and 4:1 gearing, subject to RPM quality.
        packet['speed_kmh'] = speed_kmh(left, right)
        packet['speed_online'] = packet['motor_left_ok'] and packet['motor_right_ok']
        warnings = []
        for side, label in (('left', '좌측'), ('right', '우측')):
            if packet[f'rpm_{side}_quality'] == 'NOISY':
                warnings.append(f'{label} RPM 노이즈 의심 (원본 기록)')
        if not packet['imu_ok']:
            warnings.append('IMU 미수신')
        if not packet['sas_ok']:
            warnings.append('조향각 유효성 미확인')
        if not packet['tps_ok']:
            warnings.append('TPS 미수신/범위 오류')
        packet['signal_warning'] = ' · '.join(warnings)
        return packet


def rear_records(stream, stop_event):
    """Assemble bounded complete lines across serial read timeouts."""
    parser = RearStatusParser()
    pending = bytearray()
    dropping = False
    while not stop_event.is_set():
        chunk = stream.read_until(b'\n', 2049)
        if not chunk:
            continue
        if not dropping:
            pending.extend(chunk)
            if len(pending) > 2048:
                pending.clear()
                dropping = True
        if chunk.endswith(b'\n'):
            if not dropping:
                packet = parser.parse(bytes(pending))
                if packet is not None:
                    yield packet
            pending.clear()
            dropping = False
