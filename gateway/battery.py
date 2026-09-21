"""Measured BMS channels and clearly labelled TV commands for battery analysis."""
import math
from tuning import project as tuning_project

# key, label, unit, group, validity flags (same channel contract as /tuning).
CHANNELS = (
    ('battery_power_kw', '배터리 전체 전력 · V×I', 'kW', 'power', ('bms_online',)),
    ('power_command_total_kw', '좌우 출력 요구 합계 · 실측 아님', 'kW', 'power', ('tqv_internal_online',)),
    ('power_left_kw', '좌측 출력 요구', 'kW', 'power', ('tqv_internal_online',)),
    ('power_right_kw', '우측 출력 요구', 'kW', 'power', ('tqv_internal_online',)),
    ('battery_pack_voltage_v', '팩 전압', 'V', 'voltage', ('bms_online',)),
    ('battery_current_a', '팩 전류 · BMS 부호 유지', 'A', 'current', ('bms_online',)),
    ('battery_soc_pct', '배터리 잔량', '%', 'soc', ('bms_online',)),
    ('bms_max_cell_voltage_v', '최고 셀 전압', 'V', 'cells', ('bms_online',)),
    ('bms_min_cell_voltage_v', '최저 셀 전압', 'V', 'cells', ('bms_online',)),
    ('bms_cell_delta_mv', '최고−최저 셀 전압 차이', 'mV', 'imbalance', ('bms_online',)),
    ('bms_temp_max_c', '최고 온도', '°C', 'temperature', ('bms_online',)),
    ('bms_temp_min_c', '최저 온도', '°C', 'temperature', ('bms_online',)),
    ('delta_power_kw', '좌우 차동 출력 요구 ΔP', 'kW', 'delta', ('tqv_internal_online',)),
    ('desired_yaw_rad_s', '목표 yaw rate', 'rad/s', 'yaw', ('tqv_internal_online',)),
    ('yaw_rate_rad_s', '현재 yaw rate', 'rad/s', 'yaw', ('imu_online', 'imu_ok')),
    ('yaw_error_rad_s', 'STM yaw 오차', 'rad/s', 'yaw', ('tqv_internal_online',)),
    ('tps_pct', '스로틀 개도량', '%', 'throttle', ('tps_online', 'tps_ok')),
    ('speed_kmh', '좌우 평균 RPM 환산 차속', 'km/h', 'speed', ('speed_online',)),
    ('battery_power_w', '배터리 전체 전력 · W', 'W', 'readouts', ('bms_online',)),
)
INPUTS = {c[0]: (c[0],) for c in CHANNELS if c[0].startswith(('battery_', 'bms_'))}
INPUTS.update({
    'battery_pack_voltage_v': ('battery_pack_voltage_v', 'pack_voltage_v'),
    'battery_current_a': ('battery_current_a', 'pack_current_a'),
    'battery_soc_pct': ('battery_soc_pct', 'soc_pct', 'battery_pct', 'battery_soc'),
})
STATUS_KEYS = ('bms_fault', 'bms_charge_mos_on', 'bms_discharge_mos_on',
               'bms_balancing', 'bms_charger_present', 'bms_load_present',
               'bms_cell_count', 'bms_cycle_count', 'bms_remaining_capacity_ah',
               'bms_state', 'bms_alarm_hex', 'bms_alarm_summary', 'bms_model',
               'bms_protocol', 'bms_cell_voltages_v')


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def with_power(data):
    """Use V and I from the same received packet, never mixed-age held values."""
    result = dict(data)
    voltage = data.get('battery_pack_voltage_v', data.get('pack_voltage_v'))
    current = data.get('battery_current_a', data.get('pack_current_a'))
    if finite(voltage) and voltage > 0 and finite(current) and finite(voltage * current):
        result['battery_power_w'] = voltage * current
        result['battery_power_kw'] = voltage * current / 1000.0
    elif any(key in data for key in ('battery_pack_voltage_v', 'pack_voltage_v',
                                     'battery_current_a', 'pack_current_a')):
        # A partial/invalid electrical update cannot refresh an old power value.
        result['battery_power_w'] = None
        result['battery_power_kw'] = None
    else:
        power = data.get('battery_power_kw', data.get('pack_power_kw'))
        if finite(power):
            result['battery_power_kw'] = power
            result['battery_power_w'] = power * 1000.0
    return result


def project(packet):
    values = tuning_project(packet)
    source = packet.get('_tuning_values', {}) if packet.get('_battery_fields_tracked') else with_power(packet)
    for key in INPUTS:
        value = source.get(key)
        valid = bool(packet.get('bms_online')) and finite(value)
        timing = packet.get('bms_power_timing')
        if key.startswith('battery_') and isinstance(timing, list):
            valid = valid and len(timing) == 8 and finite(timing[4]) and 0 <= timing[4] < 2000
        if key in ('battery_pack_voltage_v', 'bms_max_cell_voltage_v', 'bms_min_cell_voltage_v'):
            valid = valid and value > 0
        if key == 'battery_soc_pct':
            valid = valid and 0 <= value <= 100
        if key == 'bms_cell_delta_mv':
            valid = valid and value >= 0
        values[key] = value if valid else None
    left, right = values.get('power_left_kw'), values.get('power_right_kw')
    values['power_command_total_kw'] = left + right if finite(left) and finite(right) else None
    return {key: values.get(key) for key, *_ in CHANNELS}


def status(packet):
    """Allowlisted metadata, with unavailable states represented as null."""
    online = bool(packet.get('bms_online'))
    source = packet.get('_battery_status', packet)
    result = {'bms_online': online}
    for key in ('bms_fault', 'bms_charge_mos_on', 'bms_discharge_mos_on',
                'bms_balancing', 'bms_charger_present', 'bms_load_present'):
        result[key] = bool(source[key]) if online and isinstance(source.get(key), (bool, int)) else None
    for key in ('bms_age_ms', 'bms_cell_count', 'bms_cycle_count', 'bms_remaining_capacity_ah'):
        value = packet.get(key) if key == 'bms_age_ms' else source.get(key)
        result[key] = value if online and finite(value) else None
    for key in ('bms_state', 'bms_alarm_hex', 'bms_alarm_summary', 'bms_model', 'bms_protocol'):
        result[key] = str(source.get(key) or '')[:95] if online else ''
    cells = source.get('bms_cell_voltages_v', [])
    result['bms_cell_voltages_v'] = [v if finite(v) and v > 0 else None for v in cells[:48]] if online and isinstance(cells, list) else []
    return result
