"""Validate and de-duplicate FIFO telemetry batches; retain the vehicle clock."""
from collections import OrderedDict
from collections.abc import Mapping
from bisect import bisect_right
import re
import time

MAX_BATCH_SAMPLES = 64
MAX_BATCH_BYTES = 64 * 1024


def _uint(value, name, minimum=0):
    if type(value) is not int or not minimum <= value <= 0xffffffff:
        raise ValueError(f'{name} must be a uint32')
    return value


class RelaySamples:
    """Caller serializes prepare/accept/commit with its ingest lock."""
    def __init__(self):
        self.streams = OrderedDict()
        # Compact intervals retain exact identities, including holes filled by
        # a slower USB/HTTPS path. A high-water mark would discard those holes.
        self.received = {}

    def prepare(self, data, vehicle_id, now_ms=None):
        stream_id = data.get('stream_id')
        if not isinstance(stream_id, str) or not re.fullmatch(r'[0-9a-f]{16}', stream_id):
            raise ValueError('16 digit stream_id required')
        samples = data.get('samples')
        if not isinstance(samples, list) or not 1 <= len(samples) <= MAX_BATCH_SAMPLES:
            raise ValueError(f'batch requires 1..{MAX_BATCH_SAMPLES} samples')
        columns = data.get('columns')
        if columns is not None:
            if (not isinstance(columns, list) or not 1 <= len(columns) <= 256
                    or any(not isinstance(k, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,79}', k) for k in columns)
                    or len(set(columns)) != len(columns)
                    or not {'seq', 'timestamp_ms'}.issubset(columns)
                    or any(not isinstance(row, list) or len(row) != len(columns) for row in samples)):
                raise ValueError('invalid columns or row length')
            samples = [dict(zip(columns, row)) for row in samples]
        sent = _uint(data.get('sent_ms'), 'sent_ms')
        dropped = _uint(data.get('dropped_samples', 0), 'dropped_samples')
        queued = _uint(data.get('queue_samples', len(samples)), 'queue_samples')
        queue_bytes = _uint(data.get('queue_bytes', 0), 'queue_bytes')
        now_ms = time.time_ns() / 1e6 if now_ms is None else now_ms
        key = (vehicle_id, stream_id)
        previous = self.streams.get(key)
        # Keep one fixed clock origin per boot. An HTTP batch must not flatten
        # 200 ms sample spacing into a cluster at the server's arrival time.
        if previous is None:
            ack, clock_sent, clock_ms = 0, sent, now_ms
        else:
            ack, clock_sent, clock_ms = previous
        advance = (sent - clock_sent) & 0xffffffff
        if advance > 0x7fffffff:
            advance -= 0x100000000
        batch_ms = clock_ms + advance
        ranges = self.received.get(key, [])
        rows = []
        last_seq = 0
        last_elapsed = 0x100000000
        for sample in samples:
            if not isinstance(sample, Mapping):
                raise ValueError('sample must be a JSON object')
            seq = _uint(sample.get('seq'), 'seq', 1)
            stamp = _uint(sample.get('timestamp_ms'), 'timestamp_ms')
            elapsed = (sent - stamp) & 0xffffffff
            if seq <= last_seq or elapsed > last_elapsed or elapsed > 86400000:
                raise ValueError('samples must be ordered and no older than one day')
            last_seq, last_elapsed = seq, elapsed
            index = bisect_right(ranges, (seq, 0xffffffff)) - 1
            if index >= 0 and ranges[index][1] >= seq:
                continue
            row = dict(sample)
            row.update(sample_time_ms=round(batch_ms - elapsed),
                       sample_age_at_receive_ms=elapsed,
                       relay_stream_id=stream_id, relay_dropped_samples=dropped,
                       relay_queue_samples=queued, relay_queue_bytes=queue_bytes,
                       relay_batch_samples=len(samples))
            rows.append(row)
        # ACK exactly this batch, not a newer sequence received on another link.
        clock = (sent, batch_ms) if advance >= 0 else (clock_sent, clock_ms)
        return key, rows, last_seq, clock

    def commit(self, key, ack, clock, sequences=None):
        previous_ack = self.streams.get(key, (0,))[0]
        additions = ([(previous_ack + 1, ack)] if ack > previous_ack else []) if sequences is None else [(s, s) for s in sequences]
        merged = []
        for start, end in sorted(self.received.get(key, []) + additions):
            if merged and start <= merged[-1][1] + 1:
                merged[-1] = (merged[-1][0], max(merged[-1][1], end))
            else:
                merged.append((start, end))
        self.received[key] = merged
        self.streams[key] = (max(previous_ack, ack), *clock)
        self.streams.move_to_end(key)
        while len(self.streams) > 8:
            oldest, _ = self.streams.popitem(last=False)
            self.received.pop(oldest, None)
