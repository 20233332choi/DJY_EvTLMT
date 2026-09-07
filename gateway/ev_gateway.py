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
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Mapping

from daly_bms import DalyBmsReader


PROJECT_ROOT = Path(__file__).resolve().parents[1]
STEERING_HTML = PROJECT_ROOT / "web" / "steering" / "index.html"
DRIVER_HTML = PROJECT_ROOT / "web" / "driver" / "index.html"
TEST_PANEL_HTML = PROJECT_ROOT / "web" / "test" / "index.html"
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
        "dac_left": max(0, int(_number(data, "dac_left", "dac_l"))),
        "dac_right": max(0, int(_number(data, "dac_right", "dac_r"))),
        "power_left_kw": power_left,
        "power_right_kw": power_right,
        "delta_power_kw": delta_power,
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
        "lateral_accel_m_s2", "lat_accel", "dac_left", "dac_right", "tv_applied_pct",
    )
    SIGNAL_GROUPS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
        "speed": (("speed_kmh", "speed"), ("speed_kmh",)),
        "rpm_left": (
            ("rpm_left", "rpm_l", "cap_left", "capture_left", "motor_left_ok"),
            ("rpm_left", "cap_left", "motor_left_ok"),
        ),
        "rpm_right": (
            ("rpm_right", "rpm_r", "cap_right", "capture_right", "motor_right_ok"),
            ("rpm_right", "cap_right", "motor_right_ok"),
        ),
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
            ("yaw_rate_rad_s", "yaw_rate", "lateral_accel_m_s2", "lat_accel", "imu_ok"),
            ("yaw_rate_rad_s", "lateral_accel_m_s2", "imu_ok"),
        ),
        "rear_output": (
            ("dac_left", "dac_l", "dac_right", "dac_r", "power_left_kw", "power_right_kw",
             "delta_power_kw", "tv_requested_pct", "tv_applied_pct", "regen_requested_pct",
             "regen_applied_pct", "rear_status_seq"),
            ("dac_left", "dac_right", "power_left_kw", "power_right_kw", "delta_power_kw",
             "tv_requested_pct", "tv_applied_pct", "regen_requested_pct", "regen_applied_pct",
             "rear_status_seq", "tv_active", "regen_ready", "regen_active", "tv_limited", "regen_limited"),
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

    def update(self, data: Mapping[str, Any], now: float | None = None) -> dict[str, Any]:
        timestamp = time.monotonic() if now is None else now
        normalized = normalize_packet(data)
        with self._lock:
            packet = dict(normalized)
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
            if any(name in data for name in self.VEHICLE_INPUTS):
                self._vehicle_last_monotonic = timestamp
            self._arrivals.append(timestamp)
            cutoff = timestamp - 2.0
            self._arrivals = [arrival for arrival in self._arrivals if arrival >= cutoff]
            self._packet = packet
            return dict(packet)

    def snapshot(self, now: float | None = None) -> dict[str, Any]:
        timestamp = time.monotonic() if now is None else now
        with self._lock:
            age = None if self._vehicle_last_monotonic is None else max(0.0, timestamp - self._vehicle_last_monotonic)
            rate = 0.0
            if len(self._arrivals) > 1:
                span = self._arrivals[-1] - self._arrivals[0]
                if span > 0:
                    rate = (len(self._arrivals) - 1) / span
            result = dict(self._packet)
            for group in self.SIGNAL_GROUPS:
                last = self._signal_last.get(group)
                signal_age = None if last is None else max(0.0, timestamp - last)
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
            return result


class EVGateway:
    def __init__(self, listen_host: str, listen_port: int, pit_host: str, pit_port: int,
                 enable_control: bool = False, command_port: int = 9006,
                 relay_token: str = "") -> None:
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.pit_target = (pit_host, pit_port)
        self.store = TelemetryStore()
        self.display_test = DisplayTestState()
        self.stop_event = threading.Event()
        self.forward_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.forward_lock = threading.Lock()
        self.enable_control = enable_control
        self.command_port = command_port
        self.relay_token = relay_token.strip()
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
        self.relay_lock = threading.Lock()
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
        payload = json.dumps(self.snapshot(), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        with self.forward_lock:
            self.forward_socket.sendto(payload, self.pit_target)

    def snapshot(self, include_display_test: bool = False) -> dict[str, Any]:
        result = self.store.snapshot()
        now = time.monotonic()
        with self.transport_lock:
            active_transport = self.active_transport
            transport_last = dict(self.transport_last)
        result.update({
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
        if (
            command_state.get("command_status") == "ACKED"
            and int(result.get("config_seq", -1)) == int(command_state["command_expected_seq"])
        ):
            result["config_ack"] = True
        return result

    def relay_authorized(self, authorization: str) -> bool:
        if not self.relay_token:
            return False
        return hmac.compare_digest(authorization.strip(), f"Bearer {self.relay_token}")

    def relay_exchange(self, data: Mapping[str, Any], vehicle_id: str = "EV") -> dict[str, Any]:
        telemetry = data.get("telemetry", data)
        if not isinstance(telemetry, Mapping):
            raise ValueError("telemetry JSON object required")
        self.relay_vehicle_id = re.sub(r"[^A-Za-z0-9_.-]", "", vehicle_id)[:32] or "EV"
        self.accept(telemetry, transport="INTERNET_RELAY")
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
                        "pit_config", "live_tv", "relay_ping"
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
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.close_connection = True

        def do_GET(self) -> None:  # noqa: N802
            path = self.path.split("?", 1)[0]
            if path in {"/", "/driver", "/driver/", "/steering", "/steering/"}:
                self._send(STEERING_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {"/diagnostic", "/diagnostic/"}:
                self._send(DRIVER_HTML.read_bytes(), "text/html; charset=utf-8")
                return
            if path in {"/test", "/test/", "/controls"}:
                self._send(TEST_PANEL_HTML.read_bytes(), "text/html; charset=utf-8")
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
            if path not in {"/api/test", "/api/control", "/api/vehicle/exchange"}:
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
                maximum = 8192 if path == "/api/vehicle/exchange" else 4096
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
    parser.add_argument("--serial", metavar="PORT", help="read ESP32 JSON lines, e.g. COM8")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--front-serial", metavar="PORT",
                        help="merge Front STM CLI FRONT? data, e.g. COM11")
    parser.add_argument("--front-baud", type=int, default=115200)
    parser.add_argument("--bms-serial", metavar="PORT",
                        help="merge read-only DALY R24TS telemetry, e.g. COM5")
    parser.add_argument("--bms-baud", type=int, default=9600)
    args = parser.parse_args()

    relay_token = os.environ.get(args.relay_token_env, "")
    gateway = EVGateway(args.listen_host, args.listen_port, args.pit_host, args.pit_port,
                        enable_control=args.enable_control, command_port=args.esp_command_port,
                        relay_token=relay_token)
    if args.serial:
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
