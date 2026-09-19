"""Observe real relay health via localhost; does not send control commands."""
import argparse
import json
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=120)
args = parser.parse_args()
started = time.monotonic()
samples = []
offline = 0
while time.monotonic() - started < args.seconds:
    with urllib.request.urlopen('http://127.0.0.1:8766/api/telemetry', timeout=3) as response:
        state = json.load(response)
    sample = {key: state.get(key) for key in (
        'telemetry_transport', 'receive_rate_hz', 'stm_online', 'rear_uart_rx',
        'rear_uart_errors', 'internet_relay_tx', 'internet_relay_errors',
        'internet_relay_tls_error', 'relay_link_online', 'relay_age_ms',
        'wifi_udp_link_online', 'usb_link_online', 'stm_can_rx',
    )}
    samples.append(sample)
    offline += not bool(state.get('relay_link_online'))
    if len(samples) == 1 or len(samples) % 15 == 0:
        print(json.dumps({'elapsed_s': round(time.monotonic()-started, 1), **sample}), flush=True)
    time.sleep(1)
print(json.dumps({'summary': True, 'duration_s': round(time.monotonic()-started, 1),
                  'samples': len(samples), 'offline_samples': offline,
                  'tx_delta': samples[-1]['internet_relay_tx'] - samples[0]['internet_relay_tx'],
                  'error_delta': samples[-1]['internet_relay_errors'] - samples[0]['internet_relay_errors'],
                  'uart_error_delta': samples[-1]['rear_uart_errors'] - samples[0]['rear_uart_errors'],
                  'last': samples[-1]}), flush=True)
