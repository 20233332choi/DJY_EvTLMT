"""Read-only DALY UART telemetry used by the EV pit gateway.

Only public measurement IDs 0x90 through 0x98 are generated here.  Control
IDs and parameter writes deliberately do not exist in this module.
"""

from __future__ import annotations

import time
from typing import Any, Iterable


FRAME_LENGTH = 13
READ_IDS = tuple(range(0x90, 0x99))


def build_read_request(data_id: int) -> bytes:
    if data_id not in READ_IDS:
        raise ValueError(f"DALY data ID 0x{data_id:02X} is not read-only telemetry")
    frame = bytearray((0xA5, 0x40, data_id, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    frame[-1] = sum(frame[:-1]) & 0xFF
    return bytes(frame)


def extract_frames(buffer: bytearray) -> list[bytes]:
    frames: list[bytes] = []
    while len(buffer) >= FRAME_LENGTH:
        try:
            start = buffer.index(0xA5)
        except ValueError:
            buffer.clear()
            break
        if start:
            del buffer[:start]
        if len(buffer) < FRAME_LENGTH:
            break
        candidate = bytes(buffer[:FRAME_LENGTH])
        if candidate[1] == 0x01 and candidate[3] == 0x08 and sum(candidate[:-1]) & 0xFF == candidate[-1]:
            frames.append(candidate)
            del buffer[:FRAME_LENGTH]
        else:
            del buffer[0]
    return frames


class DalyTelemetryDecoder:
    def __init__(self) -> None:
        self.values: dict[str, Any] = {
            "bms_model": "DALY R24TS",
            "bms_protocol": "DALY UART A5",
            "bms_alarm_hex": "0000000000000000",
            "bms_alarm_summary": "NONE",
            "bms_fault": False,
            "bms_balancing": False,
            "bms_cell_voltages_v": [],
        }
        self._cells: dict[int, float] = {}
        self._temperatures: dict[int, float] = {}

    @staticmethod
    def _u16(data: bytes, offset: int) -> int:
        return (data[offset] << 8) | data[offset + 1]

    @staticmethod
    def _u32(data: bytes, offset: int) -> int:
        return int.from_bytes(data[offset:offset + 4], "big")

    def consume(self, frame: bytes) -> None:
        if len(frame) != FRAME_LENGTH or frame[0] != 0xA5 or frame[1] != 0x01:
            return
        if frame[3] != 0x08 or sum(frame[:-1]) & 0xFF != frame[-1]:
            return

        data_id = frame[2]
        data = frame[4:12]
        if data_id == 0x90:
            voltage = self._u16(data, 0) / 10.0
            current = (self._u16(data, 4) - 30000) / 10.0
            self.values.update({
                "battery_pack_voltage_v": voltage,
                "battery_current_a": current,
                "battery_power_kw": voltage * current / 1000.0,
                "battery_soc_pct": self._u16(data, 6) / 10.0,
            })
        elif data_id == 0x91:
            maximum = self._u16(data, 0) / 1000.0
            minimum = self._u16(data, 3) / 1000.0
            self.values.update({
                "bms_max_cell_voltage_v": maximum,
                "bms_max_cell_number": data[2],
                "bms_min_cell_voltage_v": minimum,
                "bms_min_cell_number": data[5],
                "bms_cell_delta_mv": max(0.0, (maximum - minimum) * 1000.0),
            })
        elif data_id == 0x92:
            self.values.update({
                "bms_temp_max_c": float(data[0] - 40),
                "bms_temp_max_sensor": data[1],
                "bms_temp_min_c": float(data[2] - 40),
                "bms_temp_min_sensor": data[3],
            })
        elif data_id == 0x93:
            state = {0: "STANDBY", 1: "CHARGING", 2: "DISCHARGING"}.get(data[0], "UNKNOWN")
            self.values.update({
                "bms_state": state,
                "bms_charge_mos_on": data[1] != 0,
                "bms_discharge_mos_on": data[2] != 0,
                "bms_life_pct": float(data[3]),
                "bms_remaining_capacity_ah": self._u32(data, 4) / 1000.0,
            })
        elif data_id == 0x94:
            self.values.update({
                "bms_cell_count": data[0],
                "bms_temp_count": data[1],
                "bms_charger_present": data[2] != 0,
                "bms_load_present": data[3] != 0,
                "bms_cycle_count": self._u16(data, 6),
            })
        elif data_id == 0x95:
            group = data[0]
            if group:
                for item in range(3):
                    millivolts = self._u16(data, 1 + item * 2)
                    cell = (group - 1) * 3 + item + 1
                    if millivolts:
                        self._cells[cell] = millivolts / 1000.0
                count = int(self.values.get("bms_cell_count", 0))
                highest = count if count > 0 else max(self._cells, default=0)
                self.values["bms_cell_voltages_v"] = [
                    self._cells[index] for index in range(1, highest + 1) if index in self._cells
                ]
        elif data_id == 0x96:
            group = data[0]
            if group:
                for item, raw in enumerate(data[1:]):
                    sensor = (group - 1) * 7 + item + 1
                    if raw != 0xFF:
                        self._temperatures[sensor] = float(raw - 40)
                count = int(self.values.get("bms_temp_count", 0))
                highest = count if count > 0 else max(self._temperatures, default=0)
                self.values["bms_temperatures_c"] = [
                    self._temperatures[index]
                    for index in range(1, highest + 1)
                    if index in self._temperatures
                ]
        elif data_id == 0x97:
            self.values["bms_balancing"] = any(data)
        elif data_id == 0x98:
            alarm_hex = data.hex().upper()
            self.values.update({
                "bms_alarm_hex": alarm_hex,
                "bms_alarm_summary": "NONE" if not any(data) else f"ALARM 0x{alarm_hex}",
                "bms_fault": any(data),
            })

    def snapshot(self) -> dict[str, Any]:
        return dict(self.values)


class DalyBmsReader:
    """Poll a pyserial-like stream without exposing any write/control API."""

    def __init__(self, stream: Any) -> None:
        self.stream = stream
        self.decoder = DalyTelemetryDecoder()
        self.buffer = bytearray()

    def _collect(self, timeout: float = 0.22) -> int:
        deadline = time.monotonic() + timeout
        received = 0
        while time.monotonic() < deadline:
            waiting = int(getattr(self.stream, "in_waiting", 0))
            chunk = self.stream.read(max(1, waiting))
            if not chunk:
                continue
            self.buffer.extend(chunk)
            for frame in extract_frames(self.buffer):
                self.decoder.consume(frame)
                received += 1
        return received

    def poll(self, ids: Iterable[int] = READ_IDS) -> dict[str, Any] | None:
        received = 0
        for data_id in ids:
            request = build_read_request(data_id)
            self.stream.write(request)
            received += self._collect()
        if received == 0:
            return None
        result = self.decoder.snapshot()
        result["bms_frames_received"] = received
        return result
