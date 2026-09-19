"""Validate and de-duplicate FIFO telemetry batches; retain the vehicle clock."""
from collections import OrderedDict
from collections.abc import Mapping
import re
import time

MAX_BATCH_SAMPLES = 4
MAX_BATCH_BYTES = 4 * 4096 + 1024


def _uint(value, name, minimum=0):
    if type(value) is not int or not minimum <= value <= 0xffffffff:
        raise ValueError(f'{name} must be a uint32')
    return value


class RelaySamples:
    """Caller serializes prepare/accept/commit with its ingest lock."""
    def __init__(self):
        self.streams = OrderedDict()

    def prepare(self, data, vehicle_id, now_ms=None):
        stream_id = data.get('stream_id')
        if not isinstance(stream_id, str) or not re.fullmatch(r'[0-9a-f]{16}', stream_id):
            raise ValueError('16 digit stream_id required')
        samples = data.get('samples')
        if not isinstance(samples, list) or not 1 <= len(samples) <= MAX_BATCH_SAMPLES:
            raise ValueError('batch requires 1..4 samples')
        sent = _uint(data.get('sent_ms'), 'sent_ms')
        dropped = _uint(data.get('dropped_samples', 0), 'dropped_samples')
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
            raise ValueError('batch clock moved backwards')
        batch_ms = clock_ms + advance
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
            if seq <= ack:
                continue
            row = dict(sample)
            row.update(sample_time_ms=round(batch_ms - elapsed),
                       sample_age_at_receive_ms=elapsed,
                       relay_stream_id=stream_id, relay_dropped_samples=dropped)
            rows.append(row)
        return key, rows, max(ack, last_seq), (sent, batch_ms)

    def commit(self, key, ack, clock):
        self.streams[key] = (ack, *clock)
        self.streams.move_to_end(key)
        while len(self.streams) > 8:
            self.streams.popitem(last=False)
