import sys, time, serial
port = sys.argv[1]
with serial.Serial(port, 115200, timeout=0.2) as stream:
    stream.dtr = False
    stream.rts = False
    deadline = time.monotonic() + 19
    while time.monotonic() < deadline:
        line = stream.readline()
        if line:
            print(line.decode('ascii', errors='replace').rstrip(), flush=True)
