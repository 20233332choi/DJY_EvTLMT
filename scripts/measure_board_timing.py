"""Read-only #TIME collection. Does not toggle reset lines or send commands."""
import argparse
import json
from pathlib import Path
import time
import serial

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--port',default='COM5')
    parser.add_argument('--seconds',type=float,default=40)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    stream=serial.Serial(port=None,baudrate=115200,timeout=.2)
    stream.dtr=False;stream.rts=False;stream.port=args.port;stream.open()
    records=[];latest={};status=[];other=[];deadline=time.monotonic()+args.seconds
    with stream:
        while time.monotonic()<deadline:
            line=stream.readline().decode('ascii',errors='replace').strip()
            if line.startswith('#TIME '):
                try:
                    row=dict(token.split('=',1) for token in line.split()[1:])
                    row={k:int(v) if k!='metric' else v for k,v in row.items()}
                    if row.get('v')!=1:continue
                    row['t_us']=(row['t_hi']<<32)|row['t_lo']
                    row['pc_receive_monotonic_ns']=time.monotonic_ns()
                    records.append(row);latest[row['metric']]=row
                except ValueError:other.append(line)
            elif line.startswith('L='):status.append(line)
            elif line:other.append(line)
    tail=records[-1] if records else {}
    summary={'port':args.port,'seconds':args.seconds,'diagnostic_records':len(records),
             'status_records':len(status),'sync_locked_records':sum(r['sync'] for r in records),
             'last_sync':{k:tail.get(k) for k in ('sync','pairs','rtt_us','bound_us','drift_ppm','front_v2','front_span_us','unmapped','negative','overrun')},
             'metrics':{k:{n:r[n] for n in ('n','min_us','mean_us','p95_upper_us','max_us')} for k,r in latest.items()},
             'limitations':['IMU ages start at estimated UART reception; internal sensor latency is unknown.',
                            'P95 is the upper histogram bucket edge; all figures use nominal Rear clock microseconds.',
                            'No synchronized Front latency conclusion unless sync=1 and front_age has samples.',
                            'Stationary observation does not validate closed-loop stability under driving.']}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'summary':summary,'records':records,'status':status,'other':other},indent=2),encoding='utf-8')
    print(json.dumps(summary,indent=2))
    if not records:raise SystemExit('No timing diagnostics received; measurement incomplete')

if __name__=='__main__':main()
