#!/usr/bin/env python3
"""Record live Assetto Corsa shared-memory telemetry as an FSK-EEM .log."""

from __future__ import annotations

import ctypes
import mmap
import sys
import time
from datetime import datetime
from pathlib import Path

from generate_ev_log import Telemetry, make_header, make_record

PROJECT_ROOT = Path(__file__).resolve().parent.parent
ASC_ROOT = PROJECT_ROOT / "ASC_TLMTSYS"
sys.path.insert(0, str(ASC_ROOT))

from ac_structs import SPageFileGraphics, SPageFilePhysics, SPageFileStatic  # noqa: E402

AC_LIVE = 2
SAMPLE_INTERVAL = 0.01  # FSK energy meter recording rate: 100 Hz


def open_shared_memory(name: str, structure: type[ctypes.Structure]) -> mmap.mmap | None:
    try:
        file_map_read = 4
        handle = ctypes.windll.kernel32.OpenFileMappingW(file_map_read, False, name)
        if not handle:
            return None
        try:
            return mmap.mmap(0, ctypes.sizeof(structure), name, access=mmap.ACCESS_READ)
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    except (OSError, AttributeError):
        return None


def read_structure(memory: mmap.mmap, structure: type[ctypes.Structure]) -> ctypes.Structure:
    memory.seek(0)
    return structure.from_buffer_copy(memory.read(ctypes.sizeof(structure)))


def clean_text(value: str) -> str:
    return value.replace("\x00", "").strip()


def wait_for_assetto_corsa() -> tuple[mmap.mmap, mmap.mmap, mmap.mmap]:
    print("Assetto Corsa 공유 메모리를 기다리는 중입니다. 게임과 주행 세션을 시작하세요.")
    while True:
        physics = open_shared_memory("Local\\acpmf_physics", SPageFilePhysics)
        graphics = open_shared_memory("Local\\acpmf_graphics", SPageFileGraphics)
        static = open_shared_memory("Local\\acpmf_static", SPageFileStatic)
        if physics and graphics and static:
            return physics, graphics, static
        for memory in (physics, graphics, static):
            if memory:
                memory.close()
        time.sleep(1.0)


def main() -> None:
    if sys.platform != "win32":
        raise SystemExit("Assetto Corsa 공유 메모리 캡처는 Windows에서만 지원합니다.")

    physics_mem, graphics_mem, static_mem = wait_for_assetto_corsa()
    try:
        static = read_structure(static_mem, SPageFileStatic)
        car = clean_text(static.carModel) or "unknown-car"
        track = clean_text(static.track) or "unknown-track"
        print(f"연결됨: {car} @ {track}")
        print("트랙에 진입할 때까지 기다리는 중...")

        while read_structure(graphics_mem, SPageFileGraphics).status != AC_LIVE:
            time.sleep(0.1)

        started_at = datetime.now()
        output_dir = Path(__file__).resolve().parent / "sessions"
        output_dir.mkdir(parents=True, exist_ok=True)
        output = output_dir / f"AC_EV_{started_at:%Y%m%d_%H%M%S}.log"

        start_clock = time.perf_counter()
        next_sample = start_clock
        records = 0
        inactive_since: float | None = None

        with output.open("wb") as stream:
            stream.write(make_header(started_at))
            print(f"기록 시작: {output}")
            print("주행 종료 후 3초가 지나면 자동 저장됩니다. 즉시 종료는 Ctrl+C입니다.")

            while True:
                now = time.perf_counter()
                graphics = read_structure(graphics_mem, SPageFileGraphics)
                if graphics.status != AC_LIVE:
                    inactive_since = inactive_since or now
                    if now - inactive_since >= 3.0:
                        break
                    time.sleep(0.05)
                    continue
                inactive_since = None

                if now < next_sample:
                    time.sleep(min(next_sample - now, 0.002))
                    continue

                physics = read_structure(physics_mem, SPageFilePhysics)
                elapsed_ms = round((now - start_clock) * 1000)

                # AC exposes ERS/KERS flags but not pack current. Blend them into the
                # brake signal so the energy-meter model shows recovery consistently.
                recovery = 0.0
                if physics.ersIsCharging:
                    recovery = max(recovery, 0.35)
                if physics.kersInput < 0:
                    recovery = max(recovery, min(abs(physics.kersInput), 1.0))

                sample = Telemetry(
                    time_ms=elapsed_ms,
                    speed_kmh=max(float(physics.speedKmh), 0.0),
                    rpm=float(physics.rpms),
                    gear=max(int(physics.gear) - 1, 0),
                    throttle=max(0.0, min(float(physics.gas), 1.0)),
                    brake=max(0.0, min(max(float(physics.brake), recovery), 1.0)),
                )
                stream.write(make_record(sample))
                records += 1
                if records % 100 == 0:
                    stream.flush()
                    print(
                        f"\r{elapsed_ms / 1000:7.1f}s | {sample.speed_kmh:6.1f} km/h"
                        f" | Gas {sample.throttle * 100:3.0f}%"
                        f" | Brake/Regen {sample.brake * 100:3.0f}%  ",
                        end="",
                        flush=True,
                    )

                next_sample += SAMPLE_INTERVAL
                if next_sample < now - SAMPLE_INTERVAL:
                    next_sample = now + SAMPLE_INTERVAL

        print(f"\n저장 완료: {output} ({records:,} records)")
        print("start_viewer.ps1 실행 후 이 파일을 선택하세요.")
    except KeyboardInterrupt:
        print("\n사용자가 기록을 종료했습니다. 현재까지의 데이터는 저장되었습니다.")
    finally:
        physics_mem.close()
        graphics_mem.close()
        static_mem.close()


if __name__ == "__main__":
    main()
