#!/usr/bin/env python3
"""Create FSK energy-meter .log files from Assetto Corsa telemetry or a demo lap."""

from __future__ import annotations

import argparse
import csv
import math
import struct
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable

MAGIC = 0xAA
TYPE_HEADER = 0
TYPE_RECORD = 1
HEADER_SIZE = 32
RECORD_SIZE = 16


@dataclass
class Telemetry:
    time_ms: int
    speed_kmh: float
    rpm: float
    gear: int
    throttle: float  # 0..1
    brake: float  # 0..1


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def normalize_pedal(value: str | float) -> float:
    number = float(value)
    return clamp(number / 100.0 if number > 1.0 else number, 0.0, 1.0)


def row_value(row: dict[str, str], *names: str, default: str = "0") -> str:
    lowered = {key.lower(): value for key, value in row.items() if key}
    for name in names:
        if name.lower() in lowered and lowered[name.lower()] not in ("", None):
            return lowered[name.lower()]
    return default


def read_csv(path: Path) -> list[Telemetry]:
    """Read ASC_TLMTSYS' Time/Speed/RPM/Gear/Gas/Brake CSV convention."""
    samples: list[Telemetry] = []
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            samples.append(
                Telemetry(
                    time_ms=int(float(row_value(row, "Time", "time_ms", "timestamp"))),
                    speed_kmh=float(row_value(row, "Speed", "speed_kmh")),
                    rpm=float(row_value(row, "RPM")),
                    gear=int(float(row_value(row, "Gear"))),
                    throttle=normalize_pedal(row_value(row, "Gas", "Throttle")),
                    brake=normalize_pedal(row_value(row, "Brake")),
                )
            )
    if not samples:
        raise ValueError(f"No telemetry rows found in {path}")
    origin = samples[0].time_ms
    for sample in samples:
        sample.time_ms -= origin
    return samples


def demo_formula_e(duration_s: float = 90.0, hz: int = 100) -> list[Telemetry]:
    """Synthetic Formula E-style street circuit lap with acceleration and braking zones."""
    result: list[Telemetry] = []
    speed = 0.0
    for index in range(int(duration_s * hz)):
        t = index / hz
        phase = t % 22.5
        if phase < 7.0:
            throttle, brake = 0.94, 0.0
        elif phase < 9.0:
            throttle, brake = 0.05, 0.88
        elif phase < 13.5:
            throttle, brake = 0.62, 0.0
        elif phase < 15.0:
            throttle, brake = 0.0, 0.72
        elif phase < 20.0:
            throttle, brake = 0.78, 0.0
        else:
            throttle, brake = 0.10, 0.48

        aero_drag = 0.000032 * speed * speed
        acceleration = 8.7 * throttle - 12.0 * brake - aero_drag
        speed = clamp(speed + acceleration * 3.6 / hz, 35.0 if t > 3 else 0.0, 320.0)
        gear = max(1, min(7, int(speed / 47.0) + 1))
        rpm = 5000 + speed * 42 + 450 * math.sin(t * 5)
        result.append(Telemetry(index * 1000 // hz, speed, rpm, gear, throttle, brake))
    return result


def energy_values(sample: Telemetry, elapsed_s: float) -> tuple[float, float, float, float]:
    """Map AC controls into a plausible meter signal within the FSK meter's range."""
    traction_kw = 350.0 * sample.throttle
    regen_kw = 350.0 * sample.brake * clamp(sample.speed_kmh / 90.0, 0.0, 1.0)
    accessory_kw = 2.2 + 0.006 * sample.speed_kmh
    power_kw = traction_kw - regen_kw + accessory_kw

    # The source logger stores 0.1 V in int16, so its representable limit is 3276.7 V.
    voltage = clamp(600.0 - max(power_kw, 0.0) * 0.095 + max(-power_kw, 0.0) * 0.035, 545.0, 615.0)
    current = clamp(power_kw * 1000.0 / voltage, -749.0, 749.0)
    lv_voltage = 13.72 - 0.12 * sample.throttle + 0.03 * math.sin(elapsed_s * 0.7)
    temperature = 31.0 + 0.035 * elapsed_s + 2.4 * sample.throttle + 0.8 * sample.brake
    return voltage, current, lv_voltage, temperature


def checksum_record(payload: bytearray) -> None:
    checksum = 0
    for offset in range(0, len(payload), 2):
        if offset != 2:
            checksum ^= struct.unpack_from("<H", payload, offset)[0]
    struct.pack_into("<H", payload, 2, checksum)


def make_header(start: datetime) -> bytes:
    payload = bytearray(HEADER_SIZE)
    struct.pack_into("<BBH", payload, 0, MAGIC, TYPE_HEADER, 0)
    struct.pack_into("<I", payload, 4, 550)
    struct.pack_into("<III", payload, 8, 0x45564552, 0x474D5431, 0x46454445)
    struct.pack_into("<hh", payload, 20, 0, 0)
    struct.pack_into(
        "<BBBBBBH",
        payload,
        24,
        start.year - 2000,
        start.month,
        start.day,
        start.hour,
        start.minute,
        start.second,
        start.microsecond // 1000,
    )
    checksum_record(payload)
    return bytes(payload)


def make_record(sample: Telemetry) -> bytes:
    voltage, current, lv_voltage, temperature = energy_values(sample, sample.time_ms / 1000.0)
    payload = bytearray(RECORD_SIZE)
    struct.pack_into("<BBHI", payload, 0, MAGIC, TYPE_RECORD, 0, sample.time_ms + 550)
    struct.pack_into(
        "<hhhh",
        payload,
        8,
        round(voltage * 10),
        round(current * 10),
        round(lv_voltage * 100),
        round(temperature * 100),
    )
    checksum_record(payload)
    return bytes(payload)


def write_log(samples: Iterable[Telemetry], output: Path, start: datetime) -> int:
    sample_list = list(samples)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as stream:
        stream.write(make_header(start))
        for sample in sample_list:
            stream.write(make_record(sample))
    return len(sample_list)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--csv", type=Path, help="ASC_TLMTSYS telemetry CSV")
    source.add_argument("--demo", action="store_true", help="generate the built-in Formula E demo (default)")
    parser.add_argument("-o", "--output", type=Path, default=Path("output/formula_e_demo.log"))
    parser.add_argument("--duration", type=float, default=90.0, help="demo duration in seconds")
    args = parser.parse_args()

    samples = read_csv(args.csv) if args.csv else demo_formula_e(args.duration)
    count = write_log(samples, args.output, datetime.now())
    print(f"Created {args.output} ({count:,} records, {args.output.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
