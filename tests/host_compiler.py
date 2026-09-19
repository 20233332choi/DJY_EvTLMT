"""Prefer the host toolchain, with a portable Zig fallback for Windows."""
import os
from pathlib import Path
import shutil


def compiler(cpp=False):
    native = shutil.which('g++' if cpp else 'gcc')
    if native:
        return [native]
    zig = Path(os.environ.get('DJY_HOST_ZIG', str(Path.home()/'telemetry-tools/host-tests/ziglang/zig.exe')))
    if not zig.is_file():
        raise RuntimeError('Install a host C/C++ compiler or set DJY_HOST_ZIG')
    return [str(zig), 'c++' if cpp else 'cc']
