#!/usr/bin/env python3
"""EV telemetry fan-out: ESP32 UDP -> steering HTTP + pit UDP."""

from __future__ import annotations

import argparse
import hmac
import json
import math
import os
import re
import socket
import sqlite3
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Mapping
from urllib.parse import parse_qs, urlparse

from daly_bms import DalyBmsReader
from stm_serial import rear_records
from tuning import CHANNELS, INPUTS as TUNING_INPUTS, TuningStream, project as tuning_project, rpm_status
from relay_samples import RelaySamples, MAX_BATCH_BYTES


PROJECT_ROOT = Path(__file__).resolve().parents[1]
STEERING_HTML = PROJECT_ROOT / "web" / "steering" / "index.html"
DRIVER_HTML = PROJECT_ROOT / "web" / "driver" / "index.html"
TEST_PANEL_HTML = PROJECT_ROOT / "web" / "test" / "index.html"
PHONE_GNSS_HTML = PROJECT_ROOT / "web" / "phone" / "index.html"
BATTERY_HTML = PROJECT_ROOT / "web" / "battery.html"
RECORDS_HTML = PROJECT_ROOT / "web" / "records" / "index.html"
TUNING_DIR = PROJECT_ROOT / "web" / "tuning"
GEAR_RATIO = 3.8
TIRE_RADIUS_M = 0.2286
VALID_FLAGS = {"CLEAR", "GREEN", "YELLOW", "RED", "STOP", "CHECKERED", "BOX"}
VALID_DRIVE_MODES = {"QUALIFYING", "RACE", "CHRG", "ATTACK"}
SAS_COUNTS = 16384
SAS_CENTER_RAW = 8192
SAS_TO_STEERING_RATIO = -0.2
TRANSPORT_PRIORITY = {
    "NONE": -1,
    "INTERNET_RELAY": 1,
    "WIFI_UDP": 2,
    "USB": 3,
}
LIVE_COMMAND_TIMEOUT_SECONDS = 0.65
FRONT_STATUS_PATTERN = re.compile(
    r"FRONT\s+sas_raw=(\d+)\s+sas_abs=([0-9.]+)\s+sas_ok=([01])\s+"
    r"tps_raw=(\d+)\s+tps_pct=([0-9.]+)\s+tps_ok=([01])\s+"
    r"tqv_pct=(\d+)\s+regen_pct=(\d+)\s+mode=(\d+)\s+flags=0x([0-9A-Fa-f]+)"
)


def _number(data: Mapping[str, Any], *names: str, default: float = 0.0) -> float:
    for name in names:
        value = data.get(name)
        if isinstance(value, bool):
            continue
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            return float(value)
    return default


def is_local_http_request(client_ip: str, headers: Mapping[str, Any]) -> bool:
    """Reject requests forwarded by ngrok even though its local agent is loopback."""
    forwarded = any(
        str(headers.get(name, "")).strip()
        for name in ("Forwarded", "X-Forwarded-For", "X-Real-IP")
    )
    return client_ip in {"127.0.0.1", "::1"} and not forwarded


def _boolean(data: Mapping[str, Any], name: str, default: bool = False) -> bool:
    value = data.get(name, default)
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    return default


def _five_percent(value: float) -> int:
    return max(0, min(100, int(round(value / 5.0)) * 5))


def _flag(data: Mapping[str, Any]) -> str:
    value = str(data.get("flag", data.get("track_flag", "CLEAR"))).strip().upper()
    return value if value in VALID_FLAGS else "CLEAR"


def _drive_mode(data: Mapping[str, Any]) -> str:
    raw = data.get("drive_mode", data.get("mode", "RACE"))
    if isinstance(raw, (int, float)) and not isinstance(raw, bool):
        raw = {0: "QUALIFYING", 1: "RACE", 2: "CHRG", 3: "ATTACK"}.get(int(raw), "RACE")
    value = str(raw).strip().upper()
    if value == "QULIFYING":
        value = "QUALIFYING"
    return value if value in VALID_DRIVE_MODES else "RACE"


def wrapped_sas_delta(raw: int, center_raw: int = SAS_CENTER_RAW) -> int:
    return ((raw - center_raw + SAS_COUNTS // 2) % SAS_COUNTS) - SAS_COUNTS // 2


def parse_front_status(line: bytes | str) -> dict[str, Any] | None:
    if isinstance(line, bytes):
        line = line.decode("ascii", errors="ignore")
    match = FRONT_STATUS_PATTERN.search(line)
    if not match:
        return None

    sas_raw = int(match.group(1))
    tps_raw = int(match.group(4))
    if not 0 <= sas_raw < SAS_COUNTS or not 0 <= tps_raw <= 4095:
        return None
    sas_relative_deg = wrapped_sas_delta(sas_raw) * 360.0 / SAS_COUNTS
    return {
        "sas_raw": sas_raw,
        "sas_absolute_deg": float(match.group(2)),
        "sas_relative_deg": sas_relative_deg,
        "sas_center_raw": SAS_CENTER_RAW,
        "sas_deg": sas_relative_deg * SAS_TO_STEERING_RATIO,
        "sas_ok": match.group(3) == "1",
        "tps_raw": tps_raw,
        "tps_pct": float(match.group(5)),
        "tps_ok": match.group(6) == "1",
        "tv_requested_pct": float(match.group(7)),
        "regen_requested_pct": float(match.group(8)),
        "drive_mode": int(match.group(9)),
        "front_control_flags": int(match.group(10), 16),
        "front_sensor_online": True,
    }


def normalize_packet(data: Mapping[str, Any]) -> dict[str, Any]:
    rpm_left = max(0, int(_number(data, "rpm_left", "rpm_l")))
    rpm_right = max(0, int(_number(data, "rpm_right", "rpm_r")))
    speed = _number(data, "speed_kmh", "speed", default=-1.0)
    if speed < 0:
        # Do not infer vehicle speed from one missing/noisy wheel.
        if (rpm_left == 0) != (rpm_right == 0):
            speed = 0.0
        else:
            motor_rpm = 0.5 * (rpm_left + rpm_right)
            wheel_rpm = motor_rpm / GEAR_RATIO
            speed = wheel_rpm * (2.0 * math.pi * TIRE_RADIUS_M) / 60.0 * 3.6

    power_left = _number(data, "power_left_kw")
    power_right = _number(data, "power_right_kw")
    delta_power = _number(data, "delta_power_kw")
    tv_percent = _number(data, "tv_percent", "torque_vector_pct", "differential_pct")
    if not any(name in data for name in ("tv_percent", "torque_vector_pct", "differential_pct")):
        total_power = abs(power_left) + abs(power_right)
        tv_percent = 100.0 * delta_power / total_power if total_power > 0.01 else 0.0

    drive_mode = _drive_mode(data)
    tv_requested = _number(data, "tv_requested_pct", "tqv_setting_pct", "tv_gain_pct")
    tv_applied = _number(data, "tv_applied_pct")
    regen_requested = _number(data, "regen_requested_pct", "regen_request_pct")
    regen_applied = _number(data, "regen_applied_pct")
    tv_limit = _number(data, "tv_limit_pct", default=100.0)
    regen_limit = _number(data, "regen_limit_pct")
    has_sas_raw = "sas_raw" in data
    sas_raw = max(0, min(SAS_COUNTS - 1, int(_number(data, "sas_raw"))))
    sas_center_raw = max(
        0,
        min(SAS_COUNTS - 1, int(_number(data, "sas_center_raw", default=SAS_CENTER_RAW))),
    )
    sas_absolute_deg = _number(
        data,
        "sas_absolute_deg",
        "sas_abs_deg",
        default=sas_raw * 360.0 / SAS_COUNTS if has_sas_raw else 0.0,
    )
    sas_relative_deg = _number(
        data,
        "sas_relative_deg",
        "sas_sensor_deg",
        default=(wrapped_sas_delta(sas_raw, sas_center_raw) * 360.0 / SAS_COUNTS)
        if has_sas_raw else 0.0,
    )
    steering_deg = _number(
        data,
        "sas_deg",
        "steering_deg",
        default=sas_relative_deg * SAS_TO_STEERING_RATIO if has_sas_raw else 0.0,
    )

    return {
        "seq": max(0, int(_number(data, "seq", "sequence"))),
        "timestamp_ms": max(0, int(_number(data, "timestamp_ms", "timestamp"))),
        "speed_kmh": round(max(0.0, speed), 3),
        "rpm_left": rpm_left,
        "rpm_right": rpm_right,
        "cap_left": max(0, int(_number(data, "cap_left", "capture_left"))),
        "cap_right": max(0, int(_number(data, "cap_right", "capture_right"))),
        "tps_raw": max(0, int(_number(data, "tps_raw"))),
        "tps_pct": max(0.0, min(100.0, _number(data, "tps_pct", "throttle_pct"))),
        "tps_ok": _boolean(data, "tps_ok", "tps_raw" in data),
        "sas_raw": sas_raw,
        "sas_center_raw": sas_center_raw,
        "sas_absolute_deg": sas_absolute_deg,
        "sas_relative_deg": sas_relative_deg,
        "sas_deg": steering_deg,
        "yaw_rate_rad_s": _number(data, "yaw_rate_rad_s", "yaw_rate"),
        "lateral_accel_m_s2": _number(data, "lateral_accel_m_s2", "lat_accel"),
        "longitudinal_accel_m_s2": _number(data, "longitudinal_accel_m_s2", "lon_accel"),
        "imu_raw_ax_m_s2": _number(data, "imu_raw_ax_m_s2"),
        "imu_raw_ay_m_s2": _number(data, "imu_raw_ay_m_s2"),
        "imu_raw_az_m_s2": _number(data, "imu_raw_az_m_s2"),
        "dac_left": max(0, int(_number(data, "dac_left", "dac_l"))),
        "dac_right": max(0, int(_number(data, "dac_right", "dac_r"))),
        "power_left_kw": power_left,
        "power_right_kw": power_right,
        "delta_power_kw": delta_power,
        "vehicle_speed_m_s": _number(data, "vehicle_speed_m_s", "vehicle_speed"),
        "desired_yaw_rad_s": _number(data, "desired_yaw_rad_s", "desired_yaw"),
        "yaw_error_rad_s": _number(data, "yaw_error_rad_s", "yaw_error"),
        "traction_scale": max(0.0, min(1.0, _number(data, "traction_scale", default=1.0))),
        "tv_percent": max(-100.0, min(100.0, tv_percent)),
        "tqv_setting_pct": max(
            0.0,
            min(100.0, _number(data, "tqv_setting_pct", "tv_gain_pct", "torque_vector_setting_pct")),
        ),
        "tv_requested_pct": max(0.0, min(100.0, tv_requested)),
        "tv_applied_pct": max(0.0, min(100.0, tv_applied)),
        "regen_requested_pct": max(0.0, min(100.0, regen_requested)),
        "regen_applied_pct": max(0.0, min(100.0, regen_applied)),
        "tv_limit_pct": max(0.0, min(100.0, tv_limit)),
        "regen_limit_pct": max(0.0, min(100.0, regen_limit)),
        "tv_ramp_pct_s": max(0.0, min(250.0, _number(data, "tv_ramp_pct_s", default=200.0))),
        "regen_ramp_pct_s": max(0.0, min(100.0, _number(data, "regen_ramp_pct_s"))),
        "drive_mode_id": {"QUALIFYING": 0, "RACE": 1, "CHRG": 2, "ATTACK": 3}[drive_mode],
        "driver_control_seq": max(0, min(255, int(_number(data, "driver_control_seq")))),
        "rear_status_seq": max(0, min(255, int(_number(data, "rear_status_seq")))),
        "config_seq": max(0, min(255, int(_number(data, "config_seq")))),
        "config_ack": _boolean(data, "config_ack"),
        "driver_control_fresh": _boolean(data, "driver_control_fresh"),
        "tv_active": _boolean(data, "tv_active"),
        "ed_active": _boolean(data, "ed_active"),
        "regen_ready": _boolean(data, "regen_ready"),
        "regen_active": _boolean(data, "regen_active", regen_applied > 0.0),
        "tv_limited": _boolean(data, "tv_limited", tv_applied + 0.5 < tv_requested),
        "regen_limited": _boolean(data, "regen_limited", regen_applied + 0.5 < regen_requested),
        "pit_adjust_allowed": _boolean(data, "pit_adjust_allowed"),
        "live_control_allowed": _boolean(data, "live_control_allowed"),
        "limit_reason": str(data.get("limit_reason", "NONE"))[:63],
        "command_source": str(data.get("command_source", "WHEEL"))[:15],
        "can_ok": _boolean(data, "can_ok"),
        "uart_ok": _boolean(data, "uart_ok"),
        "stm_online": _boolean(data, "stm_online"),
        "wifi_connected": _boolean(data, "wifi_connected"),
        "wifi_rssi_dbm": max(-127, min(0, int(_number(data, "wifi_rssi_dbm", default=-127)))),
        "internet_relay_enabled": _boolean(data, "internet_relay_enabled"),
        "internet_relay_tx": max(0, int(_number(data, "internet_relay_tx"))),
        "internet_relay_errors": max(0, int(_number(data, "internet_relay_errors"))),
        "internet_relay_commands": max(0, int(_number(data, "internet_relay_commands"))),
        "rear_uart_rx": max(0, int(_number(data, "rear_uart_rx"))),
        "rear_uart_errors": max(0, int(_number(data, "rear_uart_errors"))),
        "fault_code": max(0, int(_number(data, "fault_code", "fault"))),
        "pit_message": str(data.get("pit_message", ""))[:80],
        "battery_soc_pct": max(
            0.0,
            min(100.0, _number(data, "battery_soc_pct", "soc_pct", "battery_pct", "battery_soc")),
        ),
        "battery_pack_voltage_v": max(0.0, _number(data, "battery_pack_voltage_v", "pack_voltage_v")),
        "battery_current_a": _number(data, "battery_current_a", "pack_current_a"),
        "battery_power_kw": _number(data, "battery_power_kw", "pack_power_kw"),
        "bms_online": _boolean(
            data,
            "bms_online",
            any(name in data for name in ("battery_pack_voltage_v", "pack_voltage_v", "battery_soc_pct", "soc_pct")),
        ),
        "bms_source": str(data.get(
            "bms_source",
            "ESP / WIRELESS" if any(name in data for name in ("battery_pack_voltage_v", "pack_voltage_v")) else "NONE",
        ))[:31],
        "bms_age_ms": max(0.0, _number(data, "bms_age_ms")),
        "bms_model": str(data.get("bms_model", ""))[:31],
        "bms_protocol": str(data.get("bms_protocol", ""))[:31],
        "bms_state": str(data.get("bms_state", "UNKNOWN"))[:15],
        "bms_cell_count": max(0, min(48, int(_number(data, "bms_cell_count", "cell_count")))),
        "bms_temp_count": max(0, min(16, int(_number(data, "bms_temp_count", "temp_count")))),
        "bms_max_cell_voltage_v": max(0.0, _number(data, "bms_max_cell_voltage_v")),
        "bms_min_cell_voltage_v": max(0.0, _number(data, "bms_min_cell_voltage_v")),
        "bms_max_cell_number": max(0, min(48, int(_number(data, "bms_max_cell_number")))),
        "bms_min_cell_number": max(0, min(48, int(_number(data, "bms_min_cell_number")))),
        "bms_cell_delta_mv": max(0.0, _number(data, "bms_cell_delta_mv")),
        "bms_temp_max_c": _number(data, "bms_temp_max_c"),
        "bms_temp_min_c": _number(data, "bms_temp_min_c"),
        "bms_life_pct": max(0, min(100, int(_number(data, "bms_life_pct")))),
        "bms_charge_mos_on": _boolean(data, "bms_charge_mos_on"),
        "bms_discharge_mos_on": _boolean(data, "bms_discharge_mos_on"),
        "bms_charger_present": _boolean(data, "bms_charger_present"),
        "bms_load_present": _boolean(data, "bms_load_present"),
        "bms_balancing": _boolean(data, "bms_balancing"),
        "bms_fault": _boolean(data, "bms_fault"),
        "bms_remaining_capacity_ah": max(0.0, _number(data, "bms_remaining_capacity_ah")),
        "bms_cycle_count": max(0, int(_number(data, "bms_cycle_count"))),
        "bms_alarm_hex": str(data.get("bms_alarm_hex", ""))[:31],
        "bms_alarm_summary": str(data.get("bms_alarm_summary", "NONE"))[:95],
        "bms_cell_voltages_v": [
            max(0.0, min(10.0, float(value)))
            for value in data.get("bms_cell_voltages_v", [])[:48]
            if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))
        ] if isinstance(data.get("bms_cell_voltages_v", []), (list, tuple)) else [],
        "gnss_online": _boolean(data, "gnss_online", _boolean(data, "gps_online")),
        "gnss_fix_type": max(0, min(8, int(_number(data, "gnss_fix_type", "fix_type")))),
        "gnss_satellites": max(0, min(99, int(_number(data, "gnss_satellites", "satellites")))),
        "gnss_latitude_deg": max(-90.0, min(90.0, _number(data, "gnss_latitude_deg", "latitude"))),
        "gnss_longitude_deg": max(-180.0, min(180.0, _number(data, "gnss_longitude_deg", "longitude"))),
        "gnss_altitude_m": _number(data, "gnss_altitude_m", "altitude_m"),
        "gnss_heading_deg": _number(data, "gnss_heading_deg", "heading_deg") % 360.0,
        "gnss_hdop": max(0.0, _number(data, "gnss_hdop", "hdop")),
        "gnss_accuracy_m": max(0.0, _number(data, "gnss_accuracy_m", "accuracy_m")),
        "gnss_source": str(data.get("gnss_source", "GNSS"))[:31],
        "gnss_age_ms": max(0.0, _number(data, "gnss_age_ms")),
        "lap_time_s": max(0.0, _number(data, "lap_time_s", "current_lap_s")),
        "last_lap_s": max(0.0, _number(data, "last_lap_s")),
        "best_lap_s": max(0.0, _number(data, "best_lap_s")),
        "lap_number": max(0, int(_number(data, "lap_number", "lap"))),
        "lap_running": _boolean(data, "lap_running"),
        "flag": _flag(data),
        "adjustment_message": str(data.get("adjustment_message", data.get("adjustment", "")))[:48],
        "drive_mode": drive_mode,
        "sas_ok": _boolean(data, "sas_ok", "sas_deg" in data or "steering_deg" in data),
        "front_sensor_online": _boolean(
            data,
            "front_sensor_online",
            "sas_raw" in data or "tps_raw" in data,
        ),
        "front_source": str(data.get("front_source", "CAN / ESP"))[:31],
        "imu_ok": _boolean(
            data,
            "imu_ok",
            any(name in data for name in ("yaw_rate_rad_s", "yaw_rate", "lateral_accel_m_s2", "lat_accel")),
        ),
        "bms_ok": _boolean(
            data,
            "bms_ok",
            any(name in data for name in ("battery_soc_pct", "soc_pct", "battery_pct", "battery_soc")),
        ),
        "motor_left_ok": _boolean(data, "motor_left_ok", "rpm_left" in data or "rpm_l" in data),
        "motor_right_ok": _boolean(data, "motor_right_ok", "rpm_right" in data or "rpm_r" in data),
    }


class DisplayTestState:
    """Development-only display overrides; never writes commands to the vehicle."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._rpm: int | None = None
        self._rpm_follow_throttle = False
        self._rpm_follow_max = 12000
        self._battery: float | None = None
        self._flag: str | None = None
        self._adjustment = ""
        self._adjustment_until = 0.0
        self._lap_anchor: float | None = None
        self._lap_frozen = 0.0
        self._last_lap = 0.0
        self._best_lap = 0.0
        self._lap_number = 0
        self._drive_mode: str | None = None
        self._tv_percent: float | None = None
        self._tqv_setting: float | None = None
        self._sensor_overrides: dict[str, bool] = {}
        self._version = 0

    def apply(self, data: Mapping[str, Any]) -> dict[str, Any]:
        action = str(data.get("action", "")).strip().lower()
        now = time.monotonic()
        with self._lock:
            if action == "set_rpm":
                self._rpm = max(0, min(20000, int(_number(data, "value"))))
                self._rpm_follow_throttle = False
            elif action == "set_rpm_follow_throttle":
                self._rpm_follow_throttle = _boolean(data, "value")
                self._rpm_follow_max = max(1000, min(20000, int(_number(data, "max_rpm", default=12000))))
                self._rpm = None
            elif action == "set_battery":
                self._battery = max(0.0, min(100.0, _number(data, "value")))
            elif action == "set_flag":
                flag = str(data.get("value", "CLEAR")).strip().upper()
                if flag not in VALID_FLAGS:
                    raise ValueError("unsupported flag")
                self._flag = flag
                self._version += 1
            elif action == "set_adjustment":
                message = str(data.get("value", "")).strip()[:48]
                if not message:
                    raise ValueError("adjustment message is empty")
                self._adjustment = message
                self._adjustment_until = now + 4.0
                self._version += 1
            elif action == "lap_start":
                self._lap_anchor = now
                self._lap_frozen = 0.0
                self._lap_number += 1
            elif action == "lap_finish":
                if self._lap_anchor is not None:
                    self._lap_frozen = max(0.0, now - self._lap_anchor)
                    self._last_lap = self._lap_frozen
                    self._best_lap = (
                        self._last_lap if self._best_lap <= 0.0 else min(self._best_lap, self._last_lap)
                    )
                    self._lap_anchor = None
            elif action == "lap_reset":
                self._lap_anchor = None
                self._lap_frozen = 0.0
                self._last_lap = 0.0
                self._best_lap = 0.0
                self._lap_number = 0
            elif action == "set_mode":
                mode = str(data.get("value", "RACE")).strip().upper()
                if mode == "QULIFYING":
                    mode = "QUALIFYING"
                if mode not in VALID_DRIVE_MODES:
                    raise ValueError("unsupported drive mode")
                self._drive_mode = mode
            elif action == "set_tv":
                self._tv_percent = max(-100.0, min(100.0, _number(data, "value")))
            elif action == "set_tqv":
                self._tqv_setting = max(0.0, min(100.0, _number(data, "value")))
            elif action == "set_sensor":
                sensor = str(data.get("sensor", "")).strip().lower()
                if sensor not in {"stm_online", "can_ok", "sas_ok", "imu_ok", "bms_ok", "motor_left_ok", "motor_right_ok"}:
                    raise ValueError("unsupported sensor")
                self._sensor_overrides[sensor] = _boolean(data, "value")
            elif action == "clear_alert":
                self._flag = "CLEAR"
                self._adjustment = ""
                self._adjustment_until = 0.0
                self._version += 1
            elif action == "clear_overrides":
                self._rpm = None
                self._rpm_follow_throttle = False
                self._rpm_follow_max = 12000
                self._battery = None
                self._flag = None
                self._adjustment = ""
                self._adjustment_until = 0.0
                self._lap_anchor = None
                self._lap_frozen = 0.0
                self._last_lap = 0.0
                self._best_lap = 0.0
                self._lap_number = 0
                self._drive_mode = None
                self._tv_percent = None
                self._tqv_setting = None
                self._sensor_overrides.clear()
                self._version += 1
            else:
                raise ValueError("unsupported test action")
            return self._snapshot_locked(now)

    def _snapshot_locked(self, now: float, throttle_pct: float | None = None) -> dict[str, Any]:
        result: dict[str, Any] = {"display_command_version": self._version, "test_override": False}
        if self._rpm is not None:
            result.update({"rpm_left": self._rpm, "rpm_right": self._rpm, "test_override": True})
        elif self._rpm_follow_throttle:
            throttle = max(0.0, min(100.0, 0.0 if throttle_pct is None else float(throttle_pct)))
            rpm = round(self._rpm_follow_max * throttle / 100.0)
            result.update({
                "rpm_left": rpm,
                "rpm_right": rpm,
                "rpm_test_mode": "THROTTLE",
                "rpm_test_max": self._rpm_follow_max,
                "test_override": True,
            })
        if self._battery is not None:
            result.update({"battery_soc_pct": self._battery, "test_override": True})
        if self._flag is not None:
            result.update({"flag": self._flag, "test_override": True})
        if self._drive_mode is not None:
            result.update({"drive_mode": self._drive_mode, "test_override": True})
        if self._tv_percent is not None:
            result.update({"tv_percent": self._tv_percent, "test_override": True})
        if self._tqv_setting is not None:
            result.update({
                "tqv_setting_pct": self._tqv_setting,
                "tv_requested_pct": self._tqv_setting,
                "test_override": True,
            })
        if self._sensor_overrides:
            result.update(self._sensor_overrides)
            result["test_override"] = True
        if self._adjustment and now < self._adjustment_until:
            result.update({"adjustment_message": self._adjustment, "test_override": True})
        if self._lap_anchor is not None:
            result.update(
                {
                    "lap_time_s": max(0.0, now - self._lap_anchor),
                    "lap_running": True,
                    "lap_number": self._lap_number,
                    "test_override": True,
                },
                transport="DEMO",
            )
        elif self._lap_number or self._lap_frozen:
            result.update(
                {
                    "lap_time_s": self._lap_frozen,
                    "last_lap_s": self._lap_frozen,
                    "best_lap_s": self._best_lap,
                    "lap_running": False,
                    "lap_number": self._lap_number,
                    "test_override": True,
                }
            )
        return result

    def snapshot(self, throttle_pct: float | None = None) -> dict[str, Any]:
        with self._lock:
            return self._snapshot_locked(time.monotonic(), throttle_pct)


class TelemetryStore:
    VEHICLE_INPUTS = (
        "stm_online", "can_ok", "fault_code", "fault", "drive_mode",
        "speed_kmh", "speed", "rpm_left", "rpm_l", "rpm_right", "rpm_r", "tps_raw", "tps_pct",
        "throttle_pct", "sas_raw", "sas_deg", "steering_deg", "yaw_rate_rad_s", "yaw_rate",
        "lateral_accel_m_s2", "lat_accel", "longitudinal_accel_m_s2", "lon_accel",
        "imu_raw_ax_m_s2", "imu_raw_ay_m_s2", "imu_raw_az_m_s2", "dac_left", "dac_right", "tv_applied_pct",
    )
    SIGNAL_GROUPS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
        "pid": (("pid_kp", "pid_ki", "pid_kd", "pid_online"), ("pid_kp", "pid_ki", "pid_kd")),
        "motor_left_voltage": (("motor_left_voltage_v",), ("motor_left_voltage_v",)),
        "motor_right_voltage": (("motor_right_voltage_v",), ("motor_right_voltage_v",)),
        "speed": (("speed_kmh", "speed"), ("speed_kmh",)),
        "rpm_left": (
            ("rpm_left", "rpm_l", "cap_left", "capture_left", "motor_left_ok"),
            ("rpm_left", "cap_left", "motor_left_ok"),
        ),
        "rpm_right": (
            ("rpm_right", "rpm_r", "cap_right", "capture_right", "motor_right_ok"),
            ("rpm_right", "cap_right", "motor_right_ok"),
        ),
        "rpm_left_reported": (("rpm_left_reported",), ("rpm_left_reported", "rpm_left_quality", "rpm_left_rejected_pct")),
        "rpm_right_reported": (("rpm_right_reported",), ("rpm_right_reported", "rpm_right_quality", "rpm_right_rejected_pct")),
        "rpm_left_target": (("rpm_left_target",), ("rpm_left_target",)),
        "rpm_right_target": (("rpm_right_target",), ("rpm_right_target",)),
        "tps": (
            ("tps_raw", "tps_pct", "throttle_pct", "tps_ok"),
            ("tps_raw", "tps_pct", "tps_ok"),
        ),
        "sas": (
            ("sas_raw", "sas_deg", "steering_deg", "sas_absolute_deg", "sas_abs_deg",
             "sas_relative_deg", "sas_sensor_deg", "sas_center_raw", "sas_ok"),
            ("sas_raw", "sas_center_raw", "sas_absolute_deg", "sas_relative_deg", "sas_deg", "sas_ok"),
        ),
        "imu": (
            ("yaw_rate_rad_s", "yaw_rate", "lateral_accel_m_s2", "lat_accel",
             "longitudinal_accel_m_s2", "lon_accel", "imu_ok"),
            ("yaw_rate_rad_s", "lateral_accel_m_s2", "longitudinal_accel_m_s2", "imu_ok"),
        ),
        "rear_output": (
            ("dac_left", "dac_l", "dac_right", "dac_r", "tv_requested_pct", "tv_applied_pct",
             "regen_requested_pct", "regen_applied_pct", "rear_status_seq"),
            ("dac_left", "dac_right", "tv_requested_pct", "tv_applied_pct",
             "regen_requested_pct", "regen_applied_pct",
             "rear_status_seq", "tv_active", "regen_ready", "regen_active", "tv_limited", "regen_limited"),
        ),
        "tqv_internal": (
            ("vehicle_speed_m_s", "desired_yaw_rad_s", "yaw_error_rad_s", "delta_power_kw",
             "power_left_kw", "power_right_kw", "traction_scale", "ed_active"),
            ("vehicle_speed_m_s", "desired_yaw_rad_s", "yaw_error_rad_s", "delta_power_kw",
             "power_left_kw", "power_right_kw", "traction_scale", "ed_active"),
        ),
        "bms": (
            ("battery_soc_pct", "soc_pct", "battery_pct", "battery_pack_voltage_v", "pack_voltage_v",
             "battery_current_a", "pack_current_a", "bms_cell_voltages_v", "bms_online", "bms_ok"),
            tuple(name for name in normalize_packet({}) if name.startswith("bms_") or name.startswith("battery_")),
        ),
        "gnss": (
            ("gnss_online", "gps_online", "gnss_fix_type", "fix_type", "gnss_latitude_deg", "latitude",
             "gnss_longitude_deg", "longitude", "gnss_satellites", "satellites"),
            tuple(name for name in normalize_packet({}) if name.startswith("gnss_")),
        ),
    }

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._packet: dict[str, Any] = normalize_packet({})
        self._last_monotonic: float | None = None
        self._vehicle_last_monotonic: float | None = None
        self._received = 0
        self._lost = 0
        self._last_seq = 0
        self._arrivals: list[float] = []
        self._signal_last: dict[str, float] = {}
        self._signal_reported_online: dict[str, bool] = {}
        self._tuning_seen: dict[str, tuple[float, float | None]] = {}

    def update(self, data: Mapping[str, Any], now: float | None = None) -> dict[str, Any]:
        timestamp = time.monotonic() if now is None else now
        normalized = normalize_packet(data)
        with self._lock:
            # Keep the complete received object for the session recorder while
            # canonical normalized names remain authoritative for the UI.
            packet = dict(data)
            packet.update(normalized)
            for key, names in TUNING_INPUTS.items():
                if any(name in data for name in names):
                    valid = any(isinstance(data.get(name), (int, float))
                                and (not isinstance(data.get(name), bool) or key in ('tv_active', 'ed_active'))
                                and math.isfinite(data[name]) for name in names)
                    self._tuning_seen[key] = (timestamp, packet.get(key) if valid else None)
            for group, (input_names, output_names) in self.SIGNAL_GROUPS.items():
                present = any(name in data for name in input_names)
                if present:
                    self._signal_last[group] = timestamp
                    online_key = f"{group}_online"
                    self._signal_reported_online[group] = (
                        bool(data[online_key]) if online_key in data else True
                    )
                else:
                    for name in output_names:
                        if name in self._packet:
                            packet[name] = self._packet[name]
            sequence = int(packet["seq"])
            if sequence and self._last_seq and sequence > self._last_seq + 1:
                self._lost += sequence - self._last_seq - 1
            if sequence:
                self._last_seq = sequence
            self._received += 1
            self._last_monotonic = timestamp
            vehicle_fields_present = any(name in data for name in self.VEHICLE_INPUTS)
            stm_explicitly_offline = "stm_online" in data and not bool(data["stm_online"])
            if vehicle_fields_present and not stm_explicitly_offline:
                self._vehicle_last_monotonic = timestamp
            self._arrivals.append(timestamp)
            cutoff = timestamp - 2.0
            self._arrivals = [arrival for arrival in self._arrivals if arrival >= cutoff]
            self._packet = packet
            return dict(packet)

    def snapshot(self, now: float | None = None, include_delivery_delay: bool = True) -> dict[str, Any]:
        timestamp = time.monotonic() if now is None else now
        with self._lock:
            delay = max(0.0, _number(self._packet, 'sample_age_at_receive_ms')) / 1000.0 if include_delivery_delay else 0.0
            age = None if self._vehicle_last_monotonic is None else max(0.0, timestamp - self._vehicle_last_monotonic) + delay
            rate = 0.0
            if len(self._arrivals) > 1:
                span = self._arrivals[-1] - self._arrivals[0]
                if span > 0:
                    rate = (len(self._arrivals) - 1) / span
            result = dict(self._packet)
            if delay >= 1.5:
                result['stm_online'] = False
                result['live_control_allowed'] = False
            result['_tuning_values'] = {
                key: value if timestamp - seen + delay < 1.5 else None
                for key, (seen, value) in self._tuning_seen.items()
            }
            for group in self.SIGNAL_GROUPS:
                last = self._signal_last.get(group)
                signal_age = None if last is None else max(0.0, timestamp - last) + delay
                reported_age = result.get(f"{group}_age_ms", 0.0)
                if not isinstance(reported_age, (int, float)):
                    reported_age = 0.0
                age_ms = None if signal_age is None else max(float(reported_age), signal_age * 1000.0)
                limit_ms = 3000.0 if group == "bms" else 2000.0 if group == "gnss" else 1500.0
                result[f"{group}_online"] = (
                    age_ms is not None
                    and age_ms < limit_ms
                    and self._signal_reported_online.get(group, True)
                )
                result[f"{group}_age_ms"] = None if age_ms is None else round(age_ms, 1)
            result.update(
                {
                    "online": age is not None and age < 1.5,
                    "vehicle_online": age is not None and age < 1.5,
                    "vehicle_age_ms": None if age is None else round(age * 1000.0, 1),
                    "age_ms": None if age is None else round(age * 1000.0, 1),
                    "received_packets": self._received,
                    "lost_packets": self._lost,
                    "receive_rate_hz": round(rate, 2),
                }
            )
            if not result['vehicle_online']:
                result['stm_online'] = False
            return result


class TelemetryDatabase:
    """Session-based SQLite recorder. payload_json preserves every telemetry field."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path).resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._connection = sqlite3.connect(self.path, check_same_thread=False)
        self._connection.execute("PRAGMA journal_mode=WAL")
        self._connection.execute("PRAGMA synchronous=NORMAL")
        self._connection.execute("PRAGMA foreign_keys=ON")
        self._connection.executescript("""
            CREATE TABLE IF NOT EXISTS measurement_sessions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                started_at_utc TEXT NOT NULL,
                stopped_at_utc TEXT,
                label TEXT NOT NULL DEFAULT '',
                sample_count INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS telemetry_samples (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id INTEGER NOT NULL,
                captured_at_utc TEXT NOT NULL,
                source_timestamp_ms INTEGER,
                sequence INTEGER,
                vehicle_speed_m_s REAL,
                speed_kmh REAL,
                rpm_left INTEGER,
                rpm_right INTEGER,
                tps_raw INTEGER,
                tps_pct REAL,
                sas_raw INTEGER,
                sas_deg REAL,
                yaw_rate_rad_s REAL,
                lateral_accel_m_s2 REAL,
                longitudinal_accel_m_s2 REAL,
                imu_raw_ax_m_s2 REAL,
                imu_raw_ay_m_s2 REAL,
                imu_raw_az_m_s2 REAL,
                desired_yaw_rad_s REAL,
                yaw_error_rad_s REAL,
                delta_power_kw REAL,
                power_left_kw REAL,
                power_right_kw REAL,
                traction_scale REAL,
                tv_active INTEGER,
                ed_active INTEGER,
                dac_left INTEGER,
                dac_right INTEGER,
                battery_soc_pct REAL,
                battery_pack_voltage_v REAL,
                battery_current_a REAL,
                battery_power_kw REAL,
                gnss_latitude_deg REAL,
                gnss_longitude_deg REAL,
                gnss_altitude_m REAL,
                gnss_heading_deg REAL,
                gnss_accuracy_m REAL,
                wifi_rssi_dbm INTEGER,
                fault_code INTEGER,
                telemetry_transport TEXT,
                payload_json TEXT NOT NULL,
                FOREIGN KEY(session_id) REFERENCES measurement_sessions(id)
            );
            CREATE INDEX IF NOT EXISTS idx_telemetry_samples_session
                ON telemetry_samples(session_id, id);
        """)
        self._connection.commit()
        self._session_id: int | None = None
        self._started_epoch: float | None = None
        self._sample_count = 0

    @staticmethod
    def _utc_text(epoch: float | None = None) -> str:
        value = time.time() if epoch is None else epoch
        seconds, milliseconds = divmod(round(value * 1000), 1000)
        return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(seconds)) + f".{milliseconds:03d}Z"

    def start(self, label: str = "") -> dict[str, Any]:
        with self._lock:
            if self._session_id is not None:
                return self._status_locked()
            started = time.time()
            cursor = self._connection.execute(
                "INSERT INTO measurement_sessions(started_at_utc,label) VALUES(?,?)",
                (self._utc_text(started), str(label)[:80]),
            )
            self._connection.commit()
            self._session_id = int(cursor.lastrowid)
            self._started_epoch = started
            self._sample_count = 0
            return self._status_locked()

    def stop(self) -> dict[str, Any]:
        with self._lock:
            if self._session_id is not None:
                self._connection.execute(
                    "UPDATE measurement_sessions SET stopped_at_utc=?,sample_count=? WHERE id=?",
                    (self._utc_text(), self._sample_count, self._session_id),
                )
                self._connection.commit()
                self._session_id = None
                self._started_epoch = None
            return self._status_locked()

    def record(self, packet: Mapping[str, Any]) -> None:
        with self._lock:
            if self._session_id is None:
                return
            values = (
                self._session_id, self._utc_text(_number(packet, 'sample_time_ms') / 1000.0)
                if 'sample_time_ms' in packet else self._utc_text(), int(_number(packet, "timestamp_ms")),
                int(_number(packet, "seq")), _number(packet, "vehicle_speed_m_s"),
                _number(packet, "speed_kmh"), int(_number(packet, "rpm_left")),
                int(_number(packet, "rpm_right")), int(_number(packet, "tps_raw")),
                _number(packet, "tps_pct"), int(_number(packet, "sas_raw")),
                _number(packet, "sas_deg"), _number(packet, "yaw_rate_rad_s"),
                _number(packet, "lateral_accel_m_s2"), _number(packet, "longitudinal_accel_m_s2"),
                _number(packet, "imu_raw_ax_m_s2"), _number(packet, "imu_raw_ay_m_s2"),
                _number(packet, "imu_raw_az_m_s2"), _number(packet, "desired_yaw_rad_s"),
                _number(packet, "yaw_error_rad_s"), _number(packet, "delta_power_kw"),
                _number(packet, "power_left_kw"), _number(packet, "power_right_kw"),
                _number(packet, "traction_scale", default=1.0), int(_boolean(packet, "tv_active")),
                int(_boolean(packet, "ed_active")), int(_number(packet, "dac_left")),
                int(_number(packet, "dac_right")), _number(packet, "battery_soc_pct"),
                _number(packet, "battery_pack_voltage_v"), _number(packet, "battery_current_a"),
                _number(packet, "battery_power_kw"), _number(packet, "gnss_latitude_deg"),
                _number(packet, "gnss_longitude_deg"), _number(packet, "gnss_altitude_m"),
                _number(packet, "gnss_heading_deg"), _number(packet, "gnss_accuracy_m"),
                int(_number(packet, "wifi_rssi_dbm", default=-127.0)), int(_number(packet, "fault_code")),
                str(packet.get("telemetry_transport", ""))[:24],
                json.dumps(dict(packet), ensure_ascii=False, separators=(",", ":")),
            )
            self._connection.execute(
                """INSERT INTO telemetry_samples(
                    session_id,captured_at_utc,source_timestamp_ms,sequence,
                    vehicle_speed_m_s,speed_kmh,rpm_left,rpm_right,tps_raw,tps_pct,sas_raw,sas_deg,
                    yaw_rate_rad_s,lateral_accel_m_s2,longitudinal_accel_m_s2,
                    imu_raw_ax_m_s2,imu_raw_ay_m_s2,imu_raw_az_m_s2,
                    desired_yaw_rad_s,yaw_error_rad_s,delta_power_kw,power_left_kw,power_right_kw,
                    traction_scale,tv_active,ed_active,dac_left,dac_right,
                    battery_soc_pct,battery_pack_voltage_v,battery_current_a,battery_power_kw,
                    gnss_latitude_deg,gnss_longitude_deg,gnss_altitude_m,gnss_heading_deg,
                    gnss_accuracy_m,wifi_rssi_dbm,fault_code,telemetry_transport,payload_json
                ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                values,
            )
            self._sample_count += 1
            self._connection.execute(
                "UPDATE measurement_sessions SET sample_count=? WHERE id=?",
                (self._sample_count, self._session_id),
            )
            self._connection.commit()

    def _status_locked(self) -> dict[str, Any]:
        return {
            "recording_active": self._session_id is not None,
            "recording_session_id": self._session_id or 0,
            "recording_sample_count": self._sample_count,
            "recording_elapsed_s": 0.0 if self._started_epoch is None else max(0.0, time.time() - self._started_epoch),
            "database_path": str(self.path),
        }

    def status(self) -> dict[str, Any]:
        with self._lock:
            return self._status_locked()

    def list_sessions(self, limit: int = 100) -> list[dict[str, Any]]:
        safe_limit = max(1, min(500, int(limit)))
        with self._lock:
            rows = self._connection.execute(
                """SELECT id,started_at_utc,stopped_at_utc,label,sample_count
                   FROM measurement_sessions ORDER BY id DESC LIMIT ?""",
                (safe_limit,),
            ).fetchall()
        return [
            {
                "id": int(row[0]),
                "started_at_utc": row[1],
                "stopped_at_utc": row[2],
                "label": row[3],
                "sample_count": int(row[4]),
            }
            for row in rows
        ]

    def list_samples(self, session_id: int, limit: int = 250,
                     offset: int = 0) -> dict[str, Any]:
        safe_session_id = max(1, int(session_id))
        safe_limit = max(1, min(1000, int(limit)))
        safe_offset = max(0, int(offset))
        with self._lock:
            total_row = self._connection.execute(
                "SELECT COUNT(*) FROM telemetry_samples WHERE session_id=?",
                (safe_session_id,),
            ).fetchone()
            rows = self._connection.execute(
                """SELECT id,captured_at_utc,payload_json
                   FROM telemetry_samples WHERE session_id=?
                   ORDER BY id DESC LIMIT ? OFFSET ?""",
                (safe_session_id, safe_limit, safe_offset),
            ).fetchall()
        rows.reverse()
        samples: list[dict[str, Any]] = []
        for row in rows:
            try:
                telemetry = json.loads(row[2])
            except (TypeError, json.JSONDecodeError):
                telemetry = {}
            samples.append({
                "id": int(row[0]),
                "captured_at_utc": row[1],
                "telemetry": telemetry,
            })
        return {
            "session_id": safe_session_id,
            "total": int(total_row[0]) if total_row is not None else 0,
            "limit": safe_limit,
            "offset": safe_offset,
            "samples": samples,
        }

    def close(self) -> None:
        self.stop()
        with self._lock:
            self._connection.close()

    def tuning_samples(self, session_id: int, after: int = 0, through: int = 0,
                       limit: int = 1000) -> dict[str, Any]:
        """Keyset pages frozen at through; recording cannot shift history pages."""
        with self._lock:
            if not through:
                through = self._connection.execute(
                    "SELECT COALESCE(MAX(id),0) FROM telemetry_samples WHERE session_id=?",
                    (session_id,),
                ).fetchone()[0]
            rows = self._connection.execute(
                "SELECT id,captured_at_utc,source_timestamp_ms,payload_json FROM telemetry_samples "
                "WHERE session_id=? AND id>? AND id<=? ORDER BY id LIMIT ?",
                (session_id, after, through, max(1, min(1000, limit))),
            ).fetchall()
        from datetime import datetime
        samples = [{"id": row[0],
                    "time_ms": round(datetime.fromisoformat(row[1].replace("Z", "+00:00")).timestamp() * 1000),
                    "source_timestamp_ms": row[2], "values": tuning_project(json.loads(row[3])),
                    "status": rpm_status(json.loads(row[3]))}
                   for row in rows]
        return {"samples": samples, "through": through,
                "cursor": samples[-1]["id"] if samples else after,
                "more": bool(samples and samples[-1]["id"] < through)}


class EVGateway:
    def __init__(self, listen_host: str, listen_port: int, pit_host: str, pit_port: int,
                 enable_control: bool = False, command_port: int = 9006,
                 relay_token: str = "", database_path: str | Path | None = None) -> None:
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.pit_target = (pit_host, pit_port)
        self.store = TelemetryStore()
        self.tuning = TuningStream()
        self.display_test = DisplayTestState()
        self.stop_event = threading.Event()
        self.forward_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.forward_lock = threading.Lock()
        self.enable_control = enable_control
        self.command_port = command_port
        self.relay_token = relay_token.strip()
        self.database = TelemetryDatabase(database_path) if database_path is not None else None
        self.latest_udp_sender: tuple[str, int] | None = None
        self.serial_stream: Any | None = None
        self.serial_lock = threading.Lock()
        self.front_lock = threading.Lock()
        self.front_state: dict[str, Any] = {}
        self.front_last_monotonic: float | None = None
        self.bms_lock = threading.Lock()
        self.bms_state: dict[str, Any] = {}
        self.bms_last_monotonic: float | None = None
        self.bms_configured = False
        self.gnss_lock = threading.Lock()
        self.gnss_state: dict[str, Any] = {}
        self.gnss_last_monotonic: float | None = None
        self.command_lock = threading.Lock()
        self.command_state: dict[str, Any] = {
            "gateway_control_enabled": enable_control,
            "command_status": "IDLE",
            "command_message": "No pit command sent",
            "command_request_id": 0,
            "command_expected_seq": 0,
            "live_control_active": False,
            "live_strength_pct": 0,
            "live_limit_pct": 100,
            "live_tv_enable": False,
        }
        self.live_command: dict[str, Any] | None = None
        self.live_command_refreshed_at = 0.0
        self.transport_lock = threading.Lock()
        self.transport_last: dict[str, float] = {}
        self.active_transport = "NONE"
        self.input_mode = "WIFI_UDP"
        self.input_port = ""
        self.relay_lock = threading.Lock()
        self.relay_ingest_lock = threading.Lock()
        self.relay_samples = RelaySamples()
        self.relay_pending_command: dict[str, Any] | None = None
        self.relay_pending_until = 0.0
        self.relay_vehicle_id = ""
        self.relay_ping_baseline: int | None = None
        self.relay_ping_request_id = 0
        self.relay_ping_deadline = 0.0

    def _select_transport(self, transport: str, now: float) -> bool:
        transport = transport if transport in TRANSPORT_PRIORITY else "NONE"
        with self.transport_lock:
            self.transport_last[transport] = now
            current_last = self.transport_last.get(self.active_transport, 0.0)
            current_stale = now - current_last >= 1.0
            if (
                transport == self.active_transport
                or current_stale
                or TRANSPORT_PRIORITY[transport] > TRANSPORT_PRIORITY[self.active_transport]
            ):
                self.active_transport = transport
                return True
            return False

    def accept(self, data: Mapping[str, Any], sender: tuple[str, int] | None = None,
               transport: str = "WIFI_UDP") -> bool:
        now = time.monotonic()
        if sender is not None and transport == "WIFI_UDP":
            self.latest_udp_sender = sender
        if not self._select_transport(transport, now):
            return False
        packet = self.store.update(data, now=now)
        if packet.get("config_ack"):
            with self.command_lock:
                if int(packet["config_seq"]) == int(self.command_state["command_expected_seq"]):
                    self.command_state["command_status"] = "ACKED"
                    self.command_state["command_message"] = f"Rear STM accepted config seq {packet['config_seq']}"
        relay_commands = int(packet.get("internet_relay_commands", 0))
        with self.command_lock:
            if self.relay_ping_baseline is not None and relay_commands > self.relay_ping_baseline:
                self.command_state["command_status"] = "ACKED"
                self.command_state["command_message"] = (
                    f"ESP received internet ping seq {self.relay_ping_request_id}"
                )
                self.relay_ping_baseline = None
                self.relay_ping_deadline = 0.0
        self._forward_snapshot()
        return True

    def _forward_snapshot(self) -> None:
        # Archives keep capture-time validity, even after an internet outage.
        # Live widgets separately expire delayed samples instead of presenting
        # historical replay as a currently healthy vehicle.
        snapshot = self.snapshot(include_delivery_delay=False)
        self.tuning.append(snapshot)
        if self.database is not None:
            self.database.record(snapshot)
        payload = json.dumps(self.snapshot(), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        with self.forward_lock:
            self.forward_socket.sendto(payload, self.pit_target)

    def snapshot(self, include_display_test: bool = False, include_delivery_delay: bool = True) -> dict[str, Any]:
        result = self.store.snapshot(include_delivery_delay=include_delivery_delay)
        now = time.monotonic()
        with self.transport_lock:
            active_transport = self.active_transport
            transport_last = dict(self.transport_last)
        result.update({
            "input_mode": self.input_mode,
            "input_port": self.input_port,
            "telemetry_transport": active_transport,
            "usb_link_online": now - transport_last.get("USB", 0.0) < 1.5,
            "wifi_udp_link_online": now - transport_last.get("WIFI_UDP", 0.0) < 1.5,
            "relay_link_online": now - transport_last.get("INTERNET_RELAY", 0.0) < 1.5,
            "relay_age_ms": None if "INTERNET_RELAY" not in transport_last else
                round(max(0.0, now - transport_last["INTERNET_RELAY"]) * 1000.0, 1),
            "relay_server_enabled": bool(self.relay_token),
            "relay_vehicle_id": self.relay_vehicle_id,
        })
        with self.front_lock:
            front_state = dict(self.front_state)
            front_last = self.front_last_monotonic
        if front_last is not None:
            front_age = max(0.0, time.monotonic() - front_last)
            front_fresh = front_age < 1.0
            result["front_direct_online"] = front_fresh
            result["front_direct_age_ms"] = round(front_age * 1000.0, 1)
            if front_fresh:
                result.update(front_state)
                for key in ('tps_pct', 'sas_deg'):
                    if key in front_state:
                        result['_tuning_values'][key] = front_state[key]
                if any(name in front_state for name in ("tps_raw", "tps_pct", "tps_ok")):
                    result["tps_online"] = True
                    result["tps_age_ms"] = round(front_age * 1000.0, 1)
                if any(name in front_state for name in ("sas_raw", "sas_deg", "sas_ok")):
                    result["sas_online"] = True
                    result["sas_age_ms"] = round(front_age * 1000.0, 1)
        with self.bms_lock:
            bms_state = dict(self.bms_state)
            bms_last = self.bms_last_monotonic
            bms_configured = self.bms_configured
        if bms_configured:
            bms_age = None if bms_last is None else max(0.0, time.monotonic() - bms_last)
            bms_state["bms_online"] = bms_age is not None and bms_age < 3.0
            bms_state["bms_age_ms"] = 0.0 if bms_age is None else round(bms_age * 1000.0, 1)
            bms_state["bms_ok"] = bms_state["bms_online"] and not bool(bms_state.get("bms_fault"))
            result.update(bms_state)
        with self.gnss_lock:
            gnss_state = dict(self.gnss_state)
            gnss_last = self.gnss_last_monotonic
        if gnss_last is not None:
            gnss_age = max(0.0, time.monotonic() - gnss_last)
            gnss_state["gnss_online"] = gnss_age < 3.0
            gnss_state["gnss_age_ms"] = round(gnss_age * 1000.0, 1)
            result.update(gnss_state)
        if include_display_test:
            result.update(self.display_test.snapshot(float(result.get("tps_pct", 0.0))))
        with self.command_lock:
            if self.relay_ping_baseline is not None and now > self.relay_ping_deadline:
                self.command_state["command_status"] = "TIMEOUT"
                self.command_state["command_message"] = (
                    f"ESP did not confirm internet ping seq {self.relay_ping_request_id}"
                )
                self.relay_ping_baseline = None
                self.relay_ping_deadline = 0.0
            command_state = dict(self.command_state)
        result.update(command_state)
        if self.database is not None:
            result.update(self.database.status())
        else:
            result.update({
                "recording_active": False,
                "recording_session_id": 0,
                "recording_sample_count": 0,
                "recording_elapsed_s": 0.0,
                "database_path": "",
            })
        if (
            command_state.get("command_status") == "ACKED"
            and int(result.get("config_seq", -1)) == int(command_state["command_expected_seq"])
        ):
            result["config_ack"] = True
        return result

    def set_recording(self, active: bool, label: str = "") -> dict[str, Any]:
        if self.database is None:
            raise ValueError("telemetry database is disabled")
        if active:
            status = self.database.start(label)
            self._forward_snapshot()
            return status
        # Preserve the last known values at the requested stop boundary.
        self.database.record(self.snapshot())
        status = self.database.stop()
        self._forward_snapshot()
        return status

    def update_phone_gnss(self, data: Mapping[str, Any]) -> dict[str, Any]:
        lat = _number(data, "lat", "latitude", default=math.nan)
        lon = _number(data, "lon", "longitude", default=math.nan)
        if not math.isfinite(lat) or not -90.0 <= lat <= 90.0:
            raise ValueError("invalid latitude")
        if not math.isfinite(lon) or not -180.0 <= lon <= 180.0:
            raise ValueError("invalid longitude")

        accuracy = max(0.0, _number(data, "accuracy_m", "accuracy", default=999.0))
        if accuracy > 100.0:
            raise ValueError("GPS accuracy is worse than 100 m")
        altitude = _number(data, "altitude", "altitude_m", default=0.0)
        speed = max(0.0, _number(data, "speed_kmh", default=0.0))
        heading = _number(data, "heading", "heading_deg", default=0.0) % 360.0
        now = time.monotonic()
        state = {
            "gnss_online": True,
            "gnss_fix_type": 3 if any(name in data for name in ("altitude", "altitude_m")) else 2,
            "gnss_satellites": 0,
            "gnss_latitude_deg": lat,
            "gnss_longitude_deg": lon,
            "gnss_altitude_m": altitude,
            "gnss_heading_deg": heading,
            "gnss_hdop": 0.0,
            "gnss_accuracy_m": accuracy,
            "gnss_source": "PHONE GPS",
        }
        with self.gnss_lock:
            self.gnss_state = state
            self.gnss_last_monotonic = now
        self._forward_snapshot()
        return {
            "accepted": True,
            "accuracy_m": round(accuracy, 1),
            "speed_kmh": round(speed, 1),
            "gnss_online": True,
        }

    def relay_authorized(self, authorization: str) -> bool:
        if not self.relay_token:
            return False
        return hmac.compare_digest(authorization.strip(), f"Bearer {self.relay_token}")

    def relay_exchange(self, data: Mapping[str, Any], vehicle_id: str = "EV") -> dict[str, Any]:
        self.relay_vehicle_id = re.sub(r"[^A-Za-z0-9_.-]", "", vehicle_id)[:32] or "EV"
        ack = None
        accepted = 0
        with self.relay_ingest_lock:
            if 'samples' in data:
                key, samples, ack, clock = self.relay_samples.prepare(data, self.relay_vehicle_id)
                for sample in samples:
                    accepted += int(self.accept(sample, transport='INTERNET_RELAY'))
                self.relay_samples.commit(key, ack, clock)
            else:
                telemetry = data.get("telemetry", data)
                if not isinstance(telemetry, Mapping):
                    raise ValueError("telemetry JSON object required")
                accepted = int(self.accept(telemetry, transport="INTERNET_RELAY"))
        now = time.monotonic()
        with self.relay_lock:
            command = (
                dict(self.relay_pending_command)
                if self.relay_pending_command is not None and now <= self.relay_pending_until
                else None
            )
            if command is None:
                self.relay_pending_command = None
        return {
            "ok": True,
            "ack_seq": ack,
            "accepted_samples": accepted,
            "server_time_ms": int(time.time() * 1000.0),
            "vehicle_id": self.relay_vehicle_id,
            "command": command,
        }

    def _queue_relay_command(self, command: Mapping[str, Any]) -> bool:
        with self.transport_lock:
            relay_last = self.transport_last.get("INTERNET_RELAY", 0.0)
        if time.monotonic() - relay_last >= 1.5:
            return False
        ttl = 0.6 if command.get("type") == "live_tv" else 2.0
        with self.relay_lock:
            self.relay_pending_command = dict(command)
            self.relay_pending_until = time.monotonic() + ttl
        return True

    def front_serial_loop(self, port: str, baud: int) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise RuntimeError("front serial mode requires: python -m pip install pyserial") from exc

        while not self.stop_event.is_set():
            try:
                with serial.Serial(port=port, baudrate=baud, timeout=0.3, write_timeout=0.2) as stream:
                    stream.reset_input_buffer()
                    while not self.stop_event.is_set():
                        # Front CLI currently polls USART2 from its main loop.
                        # Pace bytes so its 100 Hz ADC/CAN work cannot overrun
                        # the single-byte UART receive register.
                        for byte in b"FRONT?\r\n":
                            stream.write(bytes((byte,)))
                            time.sleep(0.001)
                        sample = None
                        deadline = time.monotonic() + 0.35
                        while time.monotonic() < deadline and not self.stop_event.is_set():
                            line = stream.readline()
                            sample = parse_front_status(line)
                            if sample is not None:
                                break
                        if sample is not None:
                            sample["front_source"] = f"DIRECT {port}"
                            with self.front_lock:
                                self.front_state = sample
                                self.front_last_monotonic = time.monotonic()
                            self._forward_snapshot()
                        # A full FRONT? line takes about 13 ms at 115200 baud.
                        # Four updates per second are ample for the pit display
                        # and avoid stealing time from the Front 100 Hz loop.
                        self.stop_event.wait(0.25)
            except (serial.SerialException, OSError):
                self.stop_event.wait(1.0)

    def submit_config(self, data: Mapping[str, Any]) -> dict[str, Any]:
        request_id = max(0, int(_number(data, "request_id")))
        snapshot = self.store.snapshot()
        rejection = ""
        if not self.enable_control:
            rejection = "gateway started without --enable-control"
        elif not snapshot.get("online"):
            rejection = "vehicle telemetry is offline"
        elif not snapshot.get("pit_adjust_allowed"):
            rejection = "vehicle denied: assert PIT ENABLE while stationary"
        elif float(snapshot.get("speed_kmh", 999.0)) > 1.0 or float(snapshot.get("tps_pct", 999.0)) > 2.0:
            rejection = "vehicle must be stationary with throttle released"

        command = {
            "type": "pit_config",
            "request_id": request_id,
            "tv_limit_pct": max(0, min(100, round(_number(data, "tv_limit_pct")))),
            "regen_limit_pct": max(0, min(100, round(_number(data, "regen_limit_pct")))),
            "tv_ramp_pct_s": max(10, min(250, round(_number(data, "tv_ramp_pct_s", default=200)))),
            "regen_ramp_pct_s": max(5, min(100, round(_number(data, "regen_ramp_pct_s", default=50)))),
            "mode": max(0, min(3, round(_number(data, "mode", default=1)))),
            "tv_enable": _boolean(data, "tv_enable", True),
            "regen_enable": _boolean(data, "regen_enable"),
        }

        sent_to = ""
        if not rejection:
            sent_to = self._send_vehicle_command(command)
            if not rejection and not sent_to:
                rejection = "no ESP32 command path discovered"

        with self.command_lock:
            self.relay_ping_baseline = None
            self.relay_ping_deadline = 0.0
            self.command_state.update({
                "command_status": "REJECTED" if rejection else "SENT",
                "command_message": rejection or
                    f"Sent request {request_id} to {sent_to}; waiting for rear STM ACK",
                "command_request_id": request_id,
                "command_expected_seq": request_id & 0xff,
            })
        return {"ok": not rejection, **self.command_state}

    def _send_vehicle_command(self, command: Mapping[str, Any]) -> str:
        if self.input_mode == "STM_USB":
            return ""  # Rear ST-Link is strictly a read-only diagnostic input.
        encoded = (json.dumps(command, separators=(",", ":")) + "\n").encode("utf-8")
        with self.serial_lock:
            stream = self.serial_stream
            if stream is not None:
                try:
                    stream.write(encoded)
                    return "ESP32 serial"
                except Exception:
                    pass
        if self._queue_relay_command(command):
            return "ESP32 internet relay"
        if self.latest_udp_sender is not None:
            try:
                self.forward_socket.sendto(
                    encoded.rstrip(b"\n"),
                    (self.latest_udp_sender[0], self.command_port),
                )
                return f"ESP32 {self.latest_udp_sender[0]}:{self.command_port}"
            except OSError:
                pass
        return ""

    def submit_live_control(self, data: Mapping[str, Any]) -> dict[str, Any]:
        snapshot = self.store.snapshot()
        request_id = max(0, int(_number(data, "request_id"))) & 0xFF
        strength = _five_percent(_number(data, "strength_pct"))
        limit = _five_percent(_number(data, "limit_pct", default=100.0))
        enabled = _boolean(data, "tv_enable", True)
        neutral_request = not enabled or strength == 0

        rejection = ""
        if not self.enable_control:
            rejection = "gateway started without --enable-control"
        elif not snapshot.get("online"):
            rejection = "vehicle telemetry is offline"
        elif not snapshot.get("stm_online"):
            rejection = "rear STM is offline"
        elif int(snapshot.get("fault_code", 0)) != 0 and not neutral_request:
            rejection = "vehicle fault: only 50:50 command is allowed"

        command = {
            "type": "live_tv",
            "request_id": request_id,
            "strength_pct": strength,
            "limit_pct": limit,
            "tv_enable": enabled,
        }
        sent_to = ""
        if not rejection:
            sent_to = self._send_vehicle_command(command)
            if not sent_to:
                rejection = "no ESP32 command path discovered"

        with self.command_lock:
            self.relay_ping_baseline = None
            self.relay_ping_deadline = 0.0
            if not rejection:
                if neutral_request:
                    self.live_command = None
                    self.live_command_refreshed_at = 0.0
                else:
                    self.live_command = command
                    self.live_command_refreshed_at = time.monotonic()
            self.command_state.update({
                "command_status": "REJECTED" if rejection else "SENT",
                "command_message": rejection or
                    f"Sent live TV seq {request_id} to {sent_to}; waiting for Rear ACK",
                "command_request_id": request_id,
                "command_expected_seq": request_id,
                "live_control_active": bool(not rejection and not neutral_request),
                "live_strength_pct": strength,
                "live_limit_pct": limit,
                "live_tv_enable": enabled,
            })
        return {"ok": not rejection, **self.command_state}

    def submit_control(self, data: Mapping[str, Any]) -> dict[str, Any]:
        command_type = str(data.get("type", "")).strip().lower()
        if command_type in {"recording_start", "recording_stop"}:
            return {
                "ok": True,
                **self.set_recording(command_type == "recording_start", str(data.get("label", ""))),
            }
        if command_type == "relay_ping":
            request_id = max(0, int(_number(data, "request_id"))) & 0xFF
            rejection = ""
            if not self.enable_control:
                rejection = "gateway started without --enable-control"
            queued = False if rejection else self._queue_relay_command(
                {"type": "relay_ping", "request_id": request_id}
            )
            if not queued and not rejection:
                rejection = "internet relay is offline"
            baseline = int(self.store.snapshot().get("internet_relay_commands", 0))
            with self.command_lock:
                self.command_state.update({
                    "command_status": "REJECTED" if rejection else "SENT",
                    "command_message": rejection or f"Queued harmless relay ping seq {request_id}",
                    "command_request_id": request_id,
                    "command_expected_seq": request_id,
                })
                self.relay_ping_baseline = baseline if queued else None
                self.relay_ping_request_id = request_id
                self.relay_ping_deadline = time.monotonic() + 2.5 if queued else 0.0
                result = dict(self.command_state)
            return {"ok": queued, **result}
        if command_type == "live_tv":
            return self.submit_live_control(data)
        return self.submit_config(data)

    def control_heartbeat_loop(self) -> None:
        while not self.stop_event.wait(0.1):
            with self.command_lock:
                command = dict(self.live_command) if self.live_command is not None else None
                refreshed_at = self.live_command_refreshed_at
            if command is None:
                continue

            snapshot = self.store.snapshot()
            unsafe = (
                not snapshot.get("online")
                or not snapshot.get("stm_online")
                or int(snapshot.get("fault_code", 0)) != 0
            )
            expired = time.monotonic() - refreshed_at > LIVE_COMMAND_TIMEOUT_SECONDS
            if not unsafe and not expired:
                self._send_vehicle_command(command)
                continue

            neutral = {
                "type": "live_tv",
                "request_id": int(command.get("request_id", 0)) & 0xFF,
                "strength_pct": 0,
                "limit_pct": _five_percent(_number(command, "limit_pct", default=100.0)),
                "tv_enable": False,
            }
            sent_to = self._send_vehicle_command(neutral)
            with self.command_lock:
                if self.live_command == command:
                    self.live_command = None
                    self.live_command_refreshed_at = 0.0
                    self.command_state.update({
                        "command_status": "FAILSAFE",
                        "command_message": (
                            "Vehicle state forced 50:50" if unsafe else
                            "CMake heartbeat expired; requested 50:50"
                        ) + (f" via {sent_to}" if sent_to else ""),
                        "live_control_active": False,
                        "live_strength_pct": 0,
                        "live_tv_enable": False,
                    })

    def command_loop(self, host: str, port: int) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind((host, port))
            receiver.settimeout(0.5)
            while not self.stop_event.is_set():
                try:
                    payload, _sender = receiver.recvfrom(4096)
                except socket.timeout:
                    continue
                try:
                    decoded = json.loads(payload.decode("utf-8"))
                    if isinstance(decoded, dict) and decoded.get("type") in {
                        "pit_config", "live_tv", "relay_ping", "recording_start", "recording_stop"
                    }:
                        self.submit_control(decoded)
                except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
                    continue

    def udp_loop(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind((self.listen_host, self.listen_port))
            receiver.settimeout(0.5)
            while not self.stop_event.is_set():
                try:
                    payload, _sender = receiver.recvfrom(65535)
                except socket.timeout:
                    continue
                try:
                    decoded = json.loads(payload.decode("utf-8"))
                    if isinstance(decoded, dict):
                        self.accept(decoded, _sender, transport="WIFI_UDP")
                except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
                    continue

    def rear_serial_loop(self, port: str, baud: int) -> None:
        import serial

        while not self.stop_event.is_set():
            try:
                # Configure modem lines BEFORE opening: never request a reset.
                stream = serial.Serial(port=None, baudrate=baud, timeout=0.25)
                stream.dtr = False
                stream.rts = False
                stream.port = port
                stream.open()
                with stream:
                    for packet in rear_records(stream, self.stop_event):
                        # Never expose this stream to the ESP command writer.
                        self.accept(packet, transport="USB")
            except (serial.SerialException, OSError) as exc:
                print(f"Rear USB {port}: {exc}; retrying", flush=True)
                self.stop_event.wait(1.0)

    def serial_loop(self, port: str, baud: int) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise RuntimeError("serial mode requires: python -m pip install pyserial") from exc

        while not self.stop_event.is_set():
            try:
                with serial.Serial(port=port, baudrate=baud, timeout=0.25) as stream:
                    with self.serial_lock:
                        self.serial_stream = stream
                    while not self.stop_event.is_set():
                        line = stream.readline().strip()
                        if not line or line.startswith(b"#"):
                            continue
                        try:
                            decoded = json.loads(line.decode("utf-8"))
                            if isinstance(decoded, dict):
                                self.accept(decoded, transport="USB")
                        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
                            continue
            except serial.SerialException:
                with self.serial_lock:
                    self.serial_stream = None
                self.stop_event.wait(1.0)

    def bms_serial_loop(self, port: str, baud: int = 9600) -> None:
        """Merge a directly attached DALY BMS into the common wireless-ready packet."""
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise RuntimeError("BMS serial mode requires: python -m pip install pyserial") from exc

        with self.bms_lock:
            self.bms_configured = True
            self.bms_state = {
                "bms_source": f"DALY UART {port}"[:31],
                "bms_model": "DALY R24TS",
                "bms_protocol": "DALY UART A5",
                "bms_online": False,
                "bms_ok": False,
            }
        while not self.stop_event.is_set():
            try:
                stream = serial.Serial()
                stream.port = port
                stream.baudrate = baud
                stream.bytesize = serial.EIGHTBITS
                stream.parity = serial.PARITY_NONE
                stream.stopbits = serial.STOPBITS_ONE
                stream.timeout = 0.04
                stream.write_timeout = 0.5
                stream.dtr = False
                stream.rts = False
                stream.open()
                with stream:
                    reader = DalyBmsReader(stream)
                    while not self.stop_event.is_set():
                        packet = reader.poll()
                        if packet is not None:
                            packet.update({
                                "bms_source": f"DALY UART {port}"[:31],
                                "bms_online": True,
                                "bms_ok": not bool(packet.get("bms_fault")),
                            })
                            with self.bms_lock:
                                self.bms_state = packet
                                self.bms_last_monotonic = time.monotonic()
                            self._forward_snapshot()
                        self.stop_event.wait(0.25)
            except (serial.SerialException, OSError):
                with self.bms_lock:
                    self.bms_last_monotonic = None
                    self.bms_state.update({"bms_online": False, "bms_ok": False})
                self._forward_snapshot()
                self.stop_event.wait(1.0)

def make_handler(gateway: EVGateway) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _send(self, body: bytes, content_type: str, status: HTTPStatus = HTTPStatus.OK) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            # Only a fully consumed, authenticated vehicle exchange may reuse
            # its connection. Rejected requests still close (body unread).
            keep_alive = (
                self.command == "POST"
                and self.path.split("?", 1)[0] == "/api/vehicle/exchange"
                and status == HTTPStatus.OK
                and self.request_version == "HTTP/1.1"
                and self.headers.get("Connection", "").lower() != "close"
            )
            self.send_header("Connection", "keep-alive" if keep_alive else "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.close_connection = not keep_alive
            if keep_alive:
                self.connection.settimeout(5.0)

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            path = parsed.path
            if path in {"/", "/pit", "/pit/", "/tuning", "/tuning/", "/tuning/app.js"}:
                asset = "app.js" if path.endswith(".js") else "index.html"
                self._send((TUNING_DIR / asset).read_bytes(),
                           "text/javascript; charset=utf-8" if asset.endswith(".js") else "text/html; charset=utf-8")
                return
            if path in {"/battery", "/battery/"}:
                self._send(BATTERY_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            # Team sharing exposes measured channels only; raw records and
            # command/recording writes retain their existing local-only guard.
            if path.startswith("/api/tuning/"):
                try:
                    query = parse_qs(parsed.query)
                    if path == "/api/tuning/live":
                        result = gateway.tuning.read(max(0, int(query.get("after", ["0"])[0])))
                        snapshot = gateway.snapshot()
                        result.update({"now_ms": time.time_ns() // 1000000,
                                       "current": tuning_project(snapshot),
                                       "current_status": rpm_status(snapshot),
                                       "input_mode": snapshot["input_mode"],
                                       "input_port": snapshot["input_port"],
                                       "input_online": snapshot["usb_link_online"] if snapshot["input_mode"] in {"STM_USB", "ESP_USB"} else snapshot["online"],
                                       "receive_rate_hz": snapshot["receive_rate_hz"],
                                       "relay_dropped_samples": snapshot.get('relay_dropped_samples', 0),
                                       "signal_warning": str(snapshot.get("signal_warning", ""))[:240],
                                       "recording_active": snapshot["recording_active"],
                                       "recording_session_id": snapshot["recording_session_id"],
                                       "recording_sample_count": snapshot["recording_sample_count"],
                                       "can_record": is_local_http_request(self.client_address[0], self.headers),
                                       "channels": [{"key": c[0], "label": c[1], "unit": c[2], "group": c[3]} for c in CHANNELS]})
                    elif path == "/api/tuning/overview":
                        snapshot = gateway.snapshot()
                        keys = ("stm_online", "imu_online", "tqv_internal_online", "tv_active",
                                "speed_online", "speed_kmh", "bms_online", "battery_pack_voltage_v",
                                "battery_soc_pct", "battery_current_a", "bms_cell_voltages_v",
                                "gnss_online", "gnss_latitude_deg", "gnss_longitude_deg", "gnss_accuracy_m")
                        result = {key: snapshot.get(key) for key in keys}
                    elif path == "/api/tuning/sessions":
                        result = {"sessions": [] if gateway.database is None else gateway.database.list_sessions(500)}
                    elif path == "/api/tuning/samples":
                        if gateway.database is None:
                            self._send(b'{"error":"database disabled"}', "application/json", HTTPStatus.SERVICE_UNAVAILABLE)
                            return
                        sid = int(query.get("session_id", ["0"])[0])
                        after = int(query.get("after", ["0"])[0])
                        through = int(query.get("through", ["0"])[0])
                        if sid <= 0 or after < 0 or through < 0:
                            raise ValueError("invalid history cursor")
                        result = gateway.database.tuning_samples(sid, after, through)
                    else:
                        self._send(b"not found", "text/plain", HTTPStatus.NOT_FOUND)
                        return
                    self._send(json.dumps(result, ensure_ascii=False, allow_nan=False).encode("utf-8"),
                               "application/json; charset=utf-8")
                except ValueError:
                    self._send(b'{"error":"invalid query"}', "application/json", HTTPStatus.BAD_REQUEST)
                return
            if path in {"/", "/driver", "/driver/", "/steering", "/steering/"}:
                self._send(STEERING_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {"/diagnostic", "/diagnostic/"}:
                self._send(DRIVER_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {"/test", "/test/", "/controls"}:
                self._send(TEST_PANEL_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {"/phone", "/phone/", "/gps"}:
                self._send(PHONE_GNSS_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {
                "/records", "/records/", "/api/recording/status",
                "/api/recording/sessions", "/api/recording/samples",
            } and not is_local_http_request(self.client_address[0], self.headers):
                self._send(b"local pit client required", "text/plain", HTTPStatus.FORBIDDEN)
                return
            if path in {"/records", "/records/"}:
                self._send(RECORDS_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path == "/api/recording/status":
                body = json.dumps(
                    gateway.database.status() if gateway.database is not None else {
                        "recording_active": False,
                        "recording_session_id": 0,
                        "recording_sample_count": 0,
                        "recording_elapsed_s": 0.0,
                        "database_path": "",
                    },
                    ensure_ascii=False,
                ).encode("utf-8")
                self._send(body, "application/json; charset=utf-8")
                return
            if path == "/api/recording/sessions":
                sessions = [] if gateway.database is None else gateway.database.list_sessions()
                self._send(
                    json.dumps({"sessions": sessions}, ensure_ascii=False).encode("utf-8"),
                    "application/json; charset=utf-8",
                )
                return
            if path == "/api/recording/samples":
                if gateway.database is None:
                    self._send(b'{"error":"database disabled"}', "application/json",
                               HTTPStatus.SERVICE_UNAVAILABLE)
                    return
                query = parse_qs(parsed.query)
                try:
                    session_id = int(query.get("session_id", ["0"])[0])
                    limit = int(query.get("limit", ["250"])[0])
                    offset = int(query.get("offset", ["0"])[0])
                    if session_id <= 0:
                        raise ValueError("session_id must be positive")
                    body = json.dumps(
                        gateway.database.list_samples(session_id, limit, offset),
                        ensure_ascii=False,
                    ).encode("utf-8")
                    self._send(body, "application/json; charset=utf-8")
                except ValueError as exc:
                    self._send(
                        json.dumps({"error": str(exc)}, ensure_ascii=False).encode("utf-8"),
                        "application/json; charset=utf-8", HTTPStatus.BAD_REQUEST,
                    )
                return
            if path == "/api/telemetry":
                body = json.dumps(
                    gateway.snapshot(include_display_test=True), ensure_ascii=False
                ).encode("utf-8")
                self._send(body, "application/json; charset=utf-8")
                return
            if path == "/health":
                self._send(b'{"ok":true}', "application/json")
                return
            self._send(b"not found", "text/plain", HTTPStatus.NOT_FOUND)

        def do_POST(self) -> None:  # noqa: N802
            path = self.path.split("?", 1)[0]
            if path not in {"/api/test", "/api/control", "/api/vehicle/exchange", "/api/gnss"}:
                self._send(b"not found", "text/plain", HTTPStatus.NOT_FOUND)
                return
            if path in {"/api/test", "/api/control"} and not is_local_http_request(
                self.client_address[0], self.headers
            ):
                self._send(b"local pit client required", "text/plain", HTTPStatus.FORBIDDEN)
                return
            if path == "/api/vehicle/exchange" and not gateway.relay_token:
                self._send(b"relay disabled", "text/plain", HTTPStatus.SERVICE_UNAVAILABLE)
                return
            if path == "/api/vehicle/exchange" and not gateway.relay_authorized(
                self.headers.get("Authorization", "")
            ):
                self._send(b"relay authorization required", "text/plain", HTTPStatus.UNAUTHORIZED)
                return
            try:
                maximum = MAX_BATCH_BYTES if path == "/api/vehicle/exchange" else 4096
                length = max(0, int(self.headers.get("Content-Length", "0")))
                if length > maximum:
                    raise ValueError("request body too large")
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
                if not isinstance(payload, dict):
                    raise ValueError("JSON object required")
                if path == "/api/test":
                    result = gateway.display_test.apply(payload)
                elif path == "/api/control":
                    result = gateway.submit_control(payload)
                elif path == "/api/gnss":
                    result = gateway.update_phone_gnss(payload)
                else:
                    result = gateway.relay_exchange(payload, self.headers.get("X-Vehicle-ID", "EV"))
                self._send(
                    json.dumps({"ok": True, **result}, ensure_ascii=False).encode("utf-8"),
                    "application/json; charset=utf-8",
                )
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
                self._send(
                    json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False).encode("utf-8"),
                    "application/json; charset=utf-8",
                    HTTPStatus.BAD_REQUEST,
                )

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description="DJY EV steering/pit telemetry gateway")
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, default=9003)
    parser.add_argument("--pit-host", default="127.0.0.1")
    parser.add_argument("--pit-port", type=int, default=9004)
    parser.add_argument("--http-host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8766)
    parser.add_argument("--command-listen-host", default="127.0.0.1")
    parser.add_argument("--command-listen-port", type=int, default=9005)
    parser.add_argument("--esp-command-port", type=int, default=9006)
    parser.add_argument("--relay-token-env", default="DJY_EV_RELAY_TOKEN",
                        help="environment variable containing the vehicle HTTPS relay token")
    parser.add_argument("--enable-control", action="store_true",
                        help="allow stationary PIT ENABLE configuration commands")
    serial_input = parser.add_mutually_exclusive_group()
    serial_input.add_argument("--serial", metavar="PORT", help="read ESP32 JSON lines, e.g. COM8")
    serial_input.add_argument("--rear-serial", metavar="PORT", help="read-only Rear STM ST-Link text records, e.g. COM13")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--front-serial", metavar="PORT",
                        help="merge Front STM CLI FRONT? data, e.g. COM11")
    parser.add_argument("--front-baud", type=int, default=115200)
    parser.add_argument("--bms-serial", metavar="PORT",
                        help="merge read-only DALY R24TS telemetry, e.g. COM5")
    parser.add_argument("--bms-baud", type=int, default=9600)
    parser.add_argument("--database", default=str(PROJECT_ROOT / "data" / "ev_telemetry.db"),
                        help="SQLite database used by dashboard measurement sessions")
    args = parser.parse_args()
    if args.rear_serial and args.enable_control:
        parser.error("Rear ST-Link USB input is read-only; --enable-control is not allowed")
    if args.rear_serial and args.rear_serial.upper() in {
            (args.front_serial or '').upper(), (args.bms_serial or '').upper()}:
        parser.error("Rear, Front and BMS must use different serial ports")

    relay_token = "" if args.rear_serial else os.environ.get(args.relay_token_env, "")
    gateway = EVGateway(args.listen_host, args.listen_port, args.pit_host, args.pit_port,
                        enable_control=args.enable_control, command_port=args.esp_command_port,
                        relay_token=relay_token, database_path=args.database)
    if args.rear_serial:
        gateway.input_mode, gateway.input_port = "STM_USB", args.rear_serial
        source = lambda: gateway.rear_serial_loop(args.rear_serial, args.baud)
        mode = f"REAR STM USB {args.rear_serial} @ {args.baud} (READ ONLY, wireless disabled)"
    elif args.serial:
        gateway.input_mode, gateway.input_port = "ESP_USB", args.serial
        source = lambda: gateway.serial_loop(args.serial, args.baud)
        mode = f"SERIAL {args.serial} @ {args.baud}"
    else:
        source = gateway.udp_loop
        mode = f"UDP {args.listen_host}:{args.listen_port}"
    source_thread = threading.Thread(target=source, name="ev-source", daemon=True)
    source_thread.start()
    if args.front_serial:
        front_thread = threading.Thread(
            target=lambda: gateway.front_serial_loop(args.front_serial, args.front_baud),
            name="front-serial",
            daemon=True,
        )
        front_thread.start()
    if args.bms_serial:
        bms_thread = threading.Thread(
            target=lambda: gateway.bms_serial_loop(args.bms_serial, args.bms_baud),
            name="daly-bms-serial",
            daemon=True,
        )
        bms_thread.start()
    command_thread = threading.Thread(
        target=lambda: gateway.command_loop(args.command_listen_host, args.command_listen_port),
        name="ev-command", daemon=True)
    command_thread.start()
    heartbeat_thread = threading.Thread(
        target=gateway.control_heartbeat_loop,
        name="live-tv-heartbeat", daemon=True)
    heartbeat_thread.start()

    server = ThreadingHTTPServer((args.http_host, args.http_port), make_handler(gateway))
    print(f"DJY EV gateway: {mode}")
    print(f"Driver phone: http://127.0.0.1:{args.http_port}/driver")
    print(f"Driver diagnostics: http://127.0.0.1:{args.http_port}/diagnostic")
    print(f"Driver-display-only test panel: http://127.0.0.1:{args.http_port}/test")
    print(f"Pit dashboard forward: {args.pit_host}:{args.pit_port}")
    print(f"Front direct: {args.front_serial or 'DISABLED'}")
    print(f"BMS direct: {args.bms_serial or 'ESP / WIRELESS'} @ {args.bms_baud}")
    print(f"Telemetry database: {Path(args.database).resolve()}")
    print(f"Pit control: {'ENABLED' if args.enable_control else 'READ ONLY'} on UDP {args.command_listen_host}:{args.command_listen_port}")
    print(f"Vehicle HTTPS relay: {'ENABLED' if relay_token else 'DISABLED'} at /api/vehicle/exchange")
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        gateway.stop_event.set()
        server.server_close()
        gateway.forward_socket.close()
        if gateway.database is not None:
            gateway.database.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
