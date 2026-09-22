"""Read-only tuning projection; missing measurements remain null."""
import math
import threading
import time
from collections import deque

# key, label, unit, plot group, required validity flags
CHANNELS = (
    ("desired_yaw_rad_s", "목표 yaw rate", "rad/s", "yaw", ("tqv_internal_online",)),
    ("yaw_rate_rad_s", "현재 yaw rate", "rad/s", "yaw", ("imu_online", "imu_ok")),
    ("rpm_left", "좌측 측정 RPM · 품질 통과", "rpm", "rpm", ("rpm_left_online", "motor_left_ok")),
    ("rpm_right", "우측 측정 RPM · 품질 통과", "rpm", "rpm", ("rpm_right_online", "motor_right_ok")),
    ("motor_left_voltage_v", "좌측 모터 전압", "V", "voltage", ("motor_left_voltage_online",)),
    ("motor_right_voltage_v", "우측 모터 전압", "V", "voltage", ("motor_right_voltage_online",)),
    ("sas_deg", "조향각", "°", "steering", ("sas_online", "sas_ok")),
    ("tps_pct", "스로틀 개도량", "%", "throttle", ("tps_online", "tps_ok")),
    ("rpm_left_reported", "좌측 STM 보고 RPM · 검증 전", "rpm", "rpm", ("rpm_left_reported_online",)),
    ("rpm_right_reported", "우측 STM 보고 RPM · 검증 전", "rpm", "rpm", ("rpm_right_reported_online",)),
    ("rpm_left_target", "좌측 목표 RPM", "rpm", "rpm", ("rpm_left_target_online",)),
    ("rpm_right_target", "우측 목표 RPM", "rpm", "rpm", ("rpm_right_target_online",)),
    ("power_left_kw", "좌측 전력 명령", "kW", "power", ("tqv_internal_online",)),
    ("power_right_kw", "우측 전력 명령", "kW", "power", ("tqv_internal_online",)),
    ("yaw_error_rad_s", "yaw_error · STM 제어 오차", "rad/s", "yaw", ("tqv_internal_online",)),
    ("delta_power_kw", "delta_power · 최종 차동전력", "kW", "power", ("tqv_internal_online",)),
    ("vehicle_speed_m_s", "vehicle_speed · STM 계산값", "m/s", "speed_raw", ("tqv_internal_online",)),
    ("speed_kmh", "차속 · RPM 품질 통과 / 지름 45cm / 4:1", "km/h", "speed", ("speed_online",)),
    ("traction_scale", "traction_scale · 개입 배율", "0..1", "traction", ("tqv_internal_online",)),
    ("tv_active", "tv_active · 폐루프 개입", "0/1", "activity", ("tqv_internal_online",)),
    ("ed_active", "ed_active · 개루프 개입", "0/1", "activity", ("tqv_internal_online",)),
    ("pid_kp", "PID Kp · 보드 보고값", "", "settings", ("pid_online",)),
    ("pid_ki", "PID Ki · 보드 보고값", "", "settings", ("pid_online",)),
    ("pid_kd", "PID Kd · 보드 보고값", "", "settings", ("pid_online",)),
    ("battery_pack_voltage_v", "배터리 팩 전압", "V", "battery_voltage", ("bms_online",)),
    ("battery_current_a", "배터리 전류 · 방전 음수", "A", "battery_current", ("bms_online",)),
    ("battery_power_kw", "배터리 실측 전력 · 방전 음수", "kW", "battery_power", ("bms_online",)),
    ("lateral_accel_m_s2", "횡가속도", "m/s²", "acceleration", ("imu_online", "imu_ok")),
    ("longitudinal_accel_m_s2", "종가속도", "m/s²", "acceleration", ("imu_online", "imu_ok")),
)


# Only these real packet inputs may refresh a plotted measurement. Other
# members of the same sensor group must not turn a default zero into a sample.
INPUTS = {
    'desired_yaw_rad_s': ('desired_yaw_rad_s', 'desired_yaw'),
    'yaw_rate_rad_s': ('yaw_rate_rad_s', 'yaw_rate'),
    'rpm_left': ('rpm_left', 'rpm_l'), 'rpm_right': ('rpm_right', 'rpm_r'),
    'motor_left_voltage_v': ('motor_left_voltage_v',),
    'motor_right_voltage_v': ('motor_right_voltage_v',),
    'sas_deg': ('sas_deg', 'steering_deg', 'sas_raw', 'sas_relative_deg', 'sas_sensor_deg'),
    'tps_pct': ('tps_pct', 'throttle_pct', 'tps_raw'),
    'rpm_left_reported': ('rpm_left_reported',), 'rpm_right_reported': ('rpm_right_reported',),
    'rpm_left_target': ('rpm_left_target',), 'rpm_right_target': ('rpm_right_target',),
    'power_left_kw': ('power_left_kw',), 'power_right_kw': ('power_right_kw',),
    'yaw_error_rad_s': ('yaw_error_rad_s', 'yaw_error'),
    'delta_power_kw': ('delta_power_kw',),
    'vehicle_speed_m_s': ('vehicle_speed_m_s', 'vehicle_speed'),
    'speed_kmh': ('speed_kmh', 'speed'),
    'traction_scale': ('traction_scale',),
    'tv_active': ('tv_active',), 'ed_active': ('ed_active',),
    'pid_kp': ('pid_kp',), 'pid_ki': ('pid_ki',), 'pid_kd': ('pid_kd',),
    'battery_pack_voltage_v': ('battery_pack_voltage_v', 'pack_voltage_v'),
    'battery_current_a': ('battery_current_a', 'pack_current_a'),
    'battery_power_kw': ('battery_power_kw',),
    'battery_soc_pct': ('battery_soc_pct', 'soc_pct', 'battery_pct', 'battery_soc'),
    'bms_cell_delta_mv': ('bms_cell_delta_mv',),
    'bms_temp_min_c': ('bms_temp_min_c',),
    'bms_temp_max_c': ('bms_temp_max_c',),
    'lateral_accel_m_s2': ('lateral_accel_m_s2', 'lat_accel'),
    'longitudinal_accel_m_s2': ('longitudinal_accel_m_s2', 'lon_accel'),
}

def project(packet, channels=CHANNELS):
    values = {}
    for key, _, _, _, flags in channels:
        value = packet.get('_tuning_values', packet).get(key)
        if key in ('tv_active', 'ed_active') and isinstance(value, bool):
            value = int(value)
        valid = (all(packet.get(flag) for flag in flags)
                 and not isinstance(value, bool) and isinstance(value, (int, float))
                 and math.isfinite(value))
        if key.startswith('battery_') and isinstance(packet.get('bms_power_timing'), list):
            timing = packet['bms_power_timing']
            valid = valid and len(timing) == 8 and isinstance(timing[4], (int, float)) and 0 <= timing[4] < 2000
        if key.endswith('_target') and valid:
            valid = 0 <= value <= 65535
        if key in ('tv_active', 'ed_active', 'traction_scale') and valid:
            valid = 0 <= value <= 1
        if key.startswith('pid_') and valid:
            valid = 0 <= value <= 2147483.647
        values[key] = value if valid else None
    return values

def rpm_status(packet):
    """Allowlisted diagnostics shared by live, history and CSV."""
    values = project(packet)
    result = {}
    for side in ('left', 'right'):
        received = values[f'rpm_{side}_reported'] is not None
        quality = packet.get(f'rpm_{side}_quality')
        if values[f'rpm_{side}'] is not None:
            quality = 'OK'
        elif not received:
            quality = 'NO_DATA'
        elif quality not in ('NOISY', 'NO_PULSES', 'UNVERIFIED'):
            quality = 'UNVERIFIED'
        result[f'rpm_{side}_quality'] = quality
        rejected = packet.get(f'rpm_{side}_rejected_pct')
        result[f'rpm_{side}_rejected_pct'] = rejected if (received
            and isinstance(rejected, (int, float)) and not isinstance(rejected, bool)
            and math.isfinite(rejected) and 0 <= rejected <= 100) else None
    mode = packet.get('motor_command_mode')
    result['motor_command_mode'] = mode if mode in ('POWER', 'RPM') else 'UNKNOWN'
    return result


class TuningStream:
    def __init__(self, capacity=12000, projector=project, status_projector=rpm_status):
        self.projector = projector
        self.status_projector = status_projector
        self.lock = threading.Lock()
        self.samples = deque(maxlen=capacity)
        self.cursor = 0
        self.epoch = str(time.time_ns())

    def append(self, packet):
        with self.lock:
            self.cursor += 1
            self.samples.append({"id": self.cursor, "time_ms": packet.get('sample_time_ms', time.time_ns() // 1000000),
                                 "source_timestamp_ms": packet.get("timestamp_ms"),
                                 "values": self.projector(packet), "status": self.status_projector(packet)})

    def read(self, after=0):
        with self.lock:
            samples = [s for s in self.samples if s["id"] > after][:2000]
            return {"epoch": self.epoch, "samples": samples,
                    "cursor": samples[-1]["id"] if samples else self.cursor,
                    "more": bool(samples and samples[-1]["id"] < self.cursor),
                    "truncated": bool(after and self.samples and after < self.samples[0]["id"] - 1)}
