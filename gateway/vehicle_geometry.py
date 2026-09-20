"""Vehicle dimensions confirmed by the user on 2026-09-20."""
import math

GEAR_RATIO = 4.0
TIRE_DIAMETER_M = 0.45
TIRE_RADIUS_M = TIRE_DIAMETER_M / 2.0


def speed_kmh(rpm_left, rpm_right):
    return (rpm_left + rpm_right) * 0.5 / GEAR_RATIO * math.pi * TIRE_DIAMETER_M * 60.0 / 1000.0


def with_verified_speed(data):
    """Recalculate only a same-packet, explicitly valid pair of motor RPMs."""
    result = dict(data)
    left = data.get('rpm_left', data.get('rpm_l'))
    right = data.get('rpm_right', data.get('rpm_r'))
    valid = all(isinstance(v, (int, float)) and not isinstance(v, bool)
                and math.isfinite(v) and 0 <= v <= 65535 for v in (left, right))
    if valid and data.get('motor_left_ok') and data.get('motor_right_ok'):
        if 'speed_kmh' in data:
            result['speed_reported_kmh'] = data['speed_kmh']
        result['speed_kmh'] = speed_kmh(left, right)
        result['speed_online'] = data.get('rpm_left_online', True) and data.get('rpm_right_online', True)
        result['speed_geometry'] = 'DIAMETER_0.45M_GEAR_4'
    return result
