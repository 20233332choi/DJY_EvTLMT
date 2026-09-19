"""Read-only TLS timing probe: delay the client's second handshake flight."""
import json
import re
import socket
import ssl
import time
from pathlib import Path
from urllib.parse import urlsplit

config = (Path(__file__).resolve().parents[1] / 'firmware/esp32_ev_gateway/include/config.h').read_text(encoding='utf-8-sig')
host = urlsplit(re.search(r'^\s*#define\s+EV_RELAY_URL\s+"([^"]+)"', config, re.M)[1]).hostname
for cipher, delay in [('ECDHE-ECDSA-AES128-GCM-SHA256', d) for d in (0, 0.25, 0.5, 0.75)] + [('ECDHE-RSA-AES128-GCM-SHA256', 0)]:
    context = ssl.create_default_context()
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_alpn_protocols(['http/1.1'])
    context.set_ciphers(cipher)
    incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
    tls = context.wrap_bio(incoming, outgoing, server_hostname=host)
    started = time.monotonic()
    flights = 0
    try:
        with socket.create_connection((host, 443), timeout=8) as transport:
            while True:
                complete = False
                try:
                    tls.do_handshake()
                    complete = True
                except ssl.SSLWantReadError:
                    pass
                if outgoing.pending:
                    flights += 1
                    if flights == 2:
                        time.sleep(delay)
                    transport.sendall(outgoing.read())
                if complete:
                    print(json.dumps({'delay_s': delay, 'ok': True, 'elapsed_s': round(time.monotonic()-started, 3), 'cipher': tls.cipher()[0]}), flush=True)
                    break
                chunk = transport.recv(32768)
                if not chunk:
                    raise EOFError('peer closed during TLS handshake')
                incoming.write(chunk)
    except Exception as error:
        print(json.dumps({'requested_cipher': cipher, 'delay_s': delay, 'ok': False, 'elapsed_s': round(time.monotonic()-started, 3), 'error': type(error).__name__}), flush=True)
