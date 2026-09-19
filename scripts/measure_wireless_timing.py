"""Read timing carried by ESP internet relay; never opens a USB/serial port."""
import argparse
import json
from pathlib import Path
import time
import urllib.request

METRICS=('period','exec','gyro_rx_age','acc_rx_age','imu_parse','front_age','front_link',
         'rpm_l_age','rpm_r_age','gyro_period','front_acq_rx','front_acq_ctrl','front_rx_ctrl')
FIELDS=('version','ctrl','metric_id','n','min_us','mean_us','p95_upper_us','max_us',
        'sync','rtt_us','bound_us','drift_ppm','front_v2','span_us','unmapped','negative',
        'overrun','t_hi','t_lo')

def decode(packet):
    if packet.get('telemetry_transport')!='INTERNET_RELAY' or not packet.get('stm_online'):
        return None
    data=packet.get('board_timing')
    if not isinstance(data,list) or len(data)!=19 or any(type(v)is not int for v in data):return None
    if data[0]!=1 or not 0<=data[2]<len(METRICS):return None
    row=dict(zip(FIELDS,data));row['metric']=METRICS[data[2]]
    row['t_us']=(row['t_hi']<<32)|row['t_lo']
    for key in ('seq','relay_stream_id','sas_raw','sas_center_raw','sas_deg','stm_can_rx',
                'stm_can_errors','rear_uart_errors','relay_dropped_samples','sample_age_at_receive_ms',
                'relay_age_ms','fault_code','tv_active','telemetry_transport'):
        row[key]=packet.get(key)
    row['pc_receive_ns']=time.monotonic_ns()
    return row

def main():
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=90)
    p.add_argument('--url',default='http://127.0.0.1:8766/api/telemetry')
    p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    rows=[];seen=set();errors=[];latest={};end=time.monotonic()+args.seconds
    while time.monotonic()<end:
        try:
            with urllib.request.urlopen(args.url,timeout=3) as response:packet=json.load(response)
            row=decode(packet)
            if row:
                key=(row['relay_stream_id'],row['t_us'],row['metric_id'])
                if key not in seen:
                    seen.add(key);rows.append(row);latest[row['metric']]=row
        except (OSError,ValueError) as error:errors.append(str(error))
        time.sleep(.1)
    locked=[r for r in rows if r['sync']==1 and r['front_v2']==1]
    summary={'diagnostic_records':len(rows),'locked_records':len(locked),
             'metrics':{k:{f:r[f] for f in ('n','min_us','mean_us','p95_upper_us','max_us')} for k,r in latest.items()},
             'last':rows[-1] if rows else None,
             'max_observed_bound_us':max((r['bound_us'] for r in locked),default=None),
             'limitations':['Cumulative onboard statistics, not raw 100 Hz sample export.',
               'HTTP snapshots may miss diagnostic rows; each onboard statistic includes all eligible control ticks.',
               'Sensor internal latency, next Front polling wait and Rear filter/actuator latency excluded.',
               'Board-clock units; clock bound is estimated, P95 is histogram upper edge.']}
    complete=bool(locked) and all(latest.get(k,{}).get('n',0)>0 for k in METRICS[10:])
    summary['full_path_measured']=complete
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'summary':summary,'records':rows,'errors':errors},indent=2),encoding='utf-8')
    print(json.dumps(summary,indent=2))
    if not complete:raise SystemExit('Full-path wireless timing unavailable; no complete measurement claim.')

if __name__=='__main__':main()
