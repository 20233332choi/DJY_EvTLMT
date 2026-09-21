"""USB-UART columnar batches: bounded framing and CRC before DB acceptance."""
import binascii
import json
from relay_samples import MAX_BATCH_BYTES

USB_BAUD = 3_000_000


def usb_lines(stream, stop):
    pending = bytearray()
    discarding = False
    while not stop.is_set():
        chunk = stream.read(max(1, min(4096, stream.in_waiting)))
        if not chunk:
            if not stop.is_set():
                yield b'' # Let the reader resubscribe after an ESP-only reboot.
            continue
        for part in chunk.splitlines(keepends=True):
            # Only LF terminates a frame; CR is allowed for legacy JSON output.
            if not discarding:
                pending.extend(part)
                if len(pending) > MAX_BATCH_BYTES + 8:
                    pending.clear()
                    discarding = True
            if part.endswith(b'\n'):
                if not discarding:
                    yield bytes(pending)
                pending.clear()
                discarding = False


def decode_usb_line(line):
    line = line.strip()
    if not line or line.startswith(b'#'):
        return None
    if b'\t' in line:
        payload, checksum = line.rsplit(b'\t', 1)
        if len(checksum) != 4 or int(checksum, 16) != binascii.crc_hqx(payload, 0xffff):
            raise ValueError('USB telemetry CRC mismatch')
        packet = json.loads(payload)
    else:
        packet = json.loads(line) # Previous ESP firmware's plain JSON lines.
        if isinstance(packet, dict) and 'samples' in packet:
            raise ValueError('USB batch requires CRC')
    if not isinstance(packet, dict):
        raise ValueError('USB telemetry object required')
    return packet
