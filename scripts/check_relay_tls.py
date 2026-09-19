"""Read-only TLS probes of the configured relay; never print hostname or credentials."""
import json
import re
import socket
import ssl
from pathlib import Path
from urllib.parse import urlsplit

config = (Path(__file__).resolve().parents[1] / 'firmware/esp32_ev_gateway/include/config.h').read_text(encoding='utf-8-sig')
host = urlsplit(re.search(r'^\s*#define\s+EV_RELAY_URL\s+"([^"]+)"', config, re.M)[1]).hostname
for tls12, alpn in ((False,False),(True,False),(True,True)):
    context=ssl.create_default_context()
    if tls12:
        context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_2
    if alpn:
        context.set_alpn_protocols(['http/1.1'])
    try:
        with socket.create_connection((host,443),timeout=10) as raw:
            with context.wrap_socket(raw,server_hostname=host) as secured:
                secured.sendall(f'GET /health HTTP/1.1\r\nHost: {host}\r\nngrok-skip-browser-warning: 1\r\nConnection: close\r\n\r\n'.encode())
                status=secured.recv(4096).split(b'\r\n',1)[0].decode(errors='replace')
                print(json.dumps({'tls12_only':tls12,'alpn':alpn,'tls':secured.version(),'status':status}))
    except Exception as exc:
        print(json.dumps({'tls12_only':tls12,'alpn':alpn,'error_type':type(exc).__name__,'reason':str(getattr(exc,'reason',''))}))
