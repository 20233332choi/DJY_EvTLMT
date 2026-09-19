"""Collect every F1 front acquisition record without sending commands/resetting."""
import argparse
import json
import math
from pathlib import Path
import time
import serial

FIELDS=('seq','started_us','period_us','sas_us','tps_us','acquire_us',
        'enqueue_end_us','work_us','sas_raw','tps_raw','flags','can_esr','queue_dropped')

def stats(values):
    values=sorted(values)
    if not values:return {'n':0}
    return {'n':len(values),'min_us':values[0],'mean_us':sum(values)/len(values),
            'p95_us':values[math.ceil(.95*len(values))-1],'max_us':values[-1]}

def summarize(records):
    first=records[0]['started_us']
    rows=[r for r in records if ((r['started_us']-first)&0xffffffff)>=1000000 and r['period_us']]
    gaps=sum(max(0,((b['seq']-a['seq'])&0xffffffff)-1) for a,b in zip(records,records[1:]))
    result={'records':len(records),'steady_records':len(rows),'sequence_missing':gaps,
            'queue_dropped_last':records[-1]['queue_dropped'],
            'error_counts':{name:sum(bool(r['flags']&bit) for r in rows) for name,bit in
            [('sas_protocol',1),('tps_range',2),('sas_hal',4),('tps_hal',8),('can_pair_not_enqueued',16)]},
            'metrics':{k:stats([r[k] for r in rows]) for k in
            ('period_us','sas_us','tps_us','acquire_us','work_us')},
            'can_pair_enqueue_success':sum(not (r['flags']&16) for r in rows),
            'raw_ranges':{k:[min(r[k] for r in rows),max(r[k] for r in rows)] for k in ('sas_raw','tps_raw')} if rows else {}}
    result['metrics']['successful_enqueue_end_us']=stats([r['enqueue_end_us'] for r in rows if not r['flags']&16])
    result['valid_sas_read']=stats([r['sas_us'] for r in rows if not r['flags']&5])
    result['valid_tps_read']=stats([r['tps_us'] for r in rows if not r['flags']&10])
    result['work_over_10ms']=sum(r['work_us']>=10000 for r in rows)
    result['can_esr_last']=hex(records[-1]['can_esr'])
    result['limitations']=['Nominal Front clock microseconds, not certified absolute time.',
      'Read-call duration excludes physical sensor internal propagation and wait for next 100 Hz poll.',
      'enqueue success does not prove CAN delivery; no rear latency or clock synchronization test.',
      'work_us excludes final diagnostic queue copy and ISR exit overhead.',
      'A plausible SPI/ADC value alone does not prove sensor wiring or movement.']
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',default='COM3')
    p.add_argument('--seconds',type=float,default=60);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();records=[];other=[]
    stream=serial.Serial(port=None,baudrate=115200,timeout=.2)
    stream.dtr=False;stream.rts=False;stream.port=args.port;stream.open()
    deadline=time.monotonic()+args.seconds
    with stream:
        while time.monotonic()<deadline:
            line=stream.readline().decode('ascii',errors='replace').strip()
            parts=line.split(',')
            if len(parts)!=14 or parts[0]!='F1':
                if line:other.append(line)
                continue
            try:
                row={k:int(v,16 if k=='can_esr' else 10) for k,v in zip(FIELDS,parts[1:])}
                row['pc_receive_ns']=time.monotonic_ns();records.append(row)
            except ValueError:other.append(line)
    summary=summarize(records) if records else {'records':0}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'summary':summary,'records':records,'other':other},indent=2),encoding='utf-8')
    print(json.dumps(summary,indent=2))
    if not records:raise SystemExit('No valid front timing records')

if __name__=='__main__':main()
