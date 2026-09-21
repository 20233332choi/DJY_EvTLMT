#include "timing_diag.h"
#include "timing_stats.h"
#include "vehicle_clock.h"
#include "board_time_sync.h"
#include "can_comm.h"
#include "main.h"
#include <stdio.h>
extern UART_HandleTypeDef huart2;
enum {PERIOD,EXEC,GYRO_AGE,ACC_AGE,PARSE_LAG,FRONT_AGE,FRONT_LINK,RPM_L,RPM_R,IMU_PERIOD,
      FRONT_ACQ_RX,FRONT_ACQ_CTRL,FRONT_RX_CTRL,METRICS};
static TimingStat stats[METRICS];
static const char *names[METRICS]={"period","exec","gyro_rx_age","acc_rx_age","imu_parse","front_age","front_link","rpm_l_age","rpm_r_age","gyro_period",
                                 "front_acq_rx","front_acq_ctrl","front_rx_ctrl"};
static uint32_t gyro_rx,acc_rx,rpm_rx[2],previous_control,control_seq;
static bool gyro_seen,acc_seen,rpm_seen[2];
static uint32_t front_unmapped,front_negative,imu_unseen,overruns;
static uint32_t warmup_started;
static bool measuring;
void Timing_Start(void) {
    memset(stats,0,sizeof(stats));
    gyro_seen=acc_seen=false;rpm_seen[0]=rpm_seen[1]=false;
    control_seq=front_unmapped=front_negative=imu_unseen=overruns=0;
    warmup_started=VehicleClock_Us32();measuring=false;
}

void Timing_ImuPacket(uint8_t kind,uint32_t received,uint32_t parsed) {
    if(kind==0x52) {
        if(measuring && gyro_seen)TimingStat_Add(&stats[IMU_PERIOD],received-gyro_rx);
        gyro_rx=received;gyro_seen=true;
        if(measuring)TimingStat_Add(&stats[PARSE_LAG],parsed-received);
    } else if(kind==0x51){acc_rx=received;acc_seen=true;}
}
void Timing_RpmEdge(unsigned side,uint32_t captured,bool valid) {
    if(side<2){rpm_rx[side]=captured;rpm_seen[side]=valid;}
}
void Timing_ControlBegin(uint32_t now) {
    // Keep tracking inputs while startup DMA backlog drains; measure steady operation.
    if(!measuring) {
        if(now-warmup_started<1000000u)return;
        measuring=true;
    }
    if(control_seq)TimingStat_Add(&stats[PERIOD],now-previous_control);
    previous_control=now;control_seq++;
    if(gyro_seen)TimingStat_Add(&stats[GYRO_AGE],now-gyro_rx);else imu_unseen++;
    if(acc_seen)TimingStat_Add(&stats[ACC_AGE],now-acc_rx);
    for(unsigned i=0;i<2;i++)if(rpm_seen[i])TimingStat_Add(&stats[RPM_L+i],now-rpm_rx[i]);
    SensorTiming_t t=CAN_GetSensorTiming();uint32_t mapped,bound,start;
    if(t.timestamped && CAN_IsSensorFresh() && t.span_us<=5000u &&
       BoardTimeSync_Map(t.front_us,now,&mapped,&bound) &&
       BoardTimeSync_Map(t.front_us-t.span_us/2u,now,&start,&bound)) {
        int32_t age=(int32_t)(now-mapped),link=(int32_t)(t.received_us-mapped);
        int32_t start_rx=(int32_t)(t.received_us-start),start_ctrl=(int32_t)(now-start);
        int32_t rx_ctrl=(int32_t)(now-t.received_us);
        if(age>=0 && link>=0 && start_rx>=0 && start_ctrl>=0 && rx_ctrl>=0){
            TimingStat_Add(&stats[FRONT_AGE],age);TimingStat_Add(&stats[FRONT_LINK],link);
            // Map acquisition START separately to account for Front clock skew.
            // Receiver timestamp marks arrival of the complete two-frame pair.
            TimingStat_Add(&stats[FRONT_ACQ_RX],start_rx);
            TimingStat_Add(&stats[FRONT_ACQ_CTRL],start_ctrl);
            TimingStat_Add(&stats[FRONT_RX_CTRL],rx_ctrl);
        }
        else front_negative++;
    } else front_unmapped++;
}
void Timing_ControlEnd(uint32_t started) {
    if(!measuring)return;
    uint32_t duration=VehicleClock_Us32()-started;
    TimingStat_Add(&stats[EXEC],duration);if(duration>=10000u)overruns++;
}
bool Timing_ReadRelay(uint32_t fields[19]) {
    static unsigned metric;
    if(!measuring){memset(fields,0,19u*sizeof(*fields));return false;}
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint64_t clock=VehicleClock_NowUs();
    BoardTimeSyncStatus sy=BoardTimeSync_Status((uint32_t)clock);
    TimingStat stat=stats[metric];SensorTiming_t input=CAN_GetSensorTiming();
    uint32_t seq=control_seq,fm=front_unmapped,fn=front_negative,ov=overruns;
    __set_PRIMASK(mask);
    // Same 19 fields/scales as the old ts= extension; drift word is signed.
    uint32_t next[19]={1u,seq,metric,stat.n,
      stat.n?stat.min:UINT32_MAX,stat.n?(uint32_t)(stat.sum/stat.n):UINT32_MAX,
      TimingStat_P95(&stat),stat.n?stat.max:UINT32_MAX,
      sy.valid,sy.rtt_us,sy.bound_us,(uint32_t)sy.drift_ppm,
      input.timestamped,input.span_us,fm,fn,ov,(uint32_t)(clock>>32),(uint32_t)clock};
    memcpy(fields,next,sizeof(next));
    metric=(metric+1)%METRICS;return true;
}
void Timing_Publish(void) {
    static uint32_t previous;
    static unsigned metric;
    static char line[600];
    uint32_t now=VehicleClock_Us32();
    if(!measuring || now-previous<190000u || huart2.gState!=HAL_UART_STATE_READY)return;
    uint32_t mask=__get_PRIMASK();__disable_irq();
    BoardTimeSyncStatus sy=BoardTimeSync_Status(now);
    TimingStat stat=stats[metric];uint32_t seq=control_seq;
    uint32_t fm=front_unmapped,fn=front_negative,iu=imu_unseen,ov=overruns;
    SensorTiming_t input=CAN_GetSensorTiming();
    uint64_t clock=VehicleClock_NowUs();
    __set_PRIMASK(mask);
    // One metric per telemetry cycle. Each statistic includes all 100 Hz ticks.
    // rx_age excludes the IMU's undocumented internal sample/filter delay.
    int n=snprintf(line,sizeof(line),
      "#TIME v=1 t_hi=%lu t_lo=%lu ctrl=%lu sync=%lu pairs=%lu rtt_us=%lu bound_us=%lu drift_ppm=%ld sync_age_us=%lu reject=%lu front_v2=%u front_span_us=%u unmapped=%lu negative=%lu imu_unseen=%lu overrun=%lu metric=%s n=%lu min_us=%lu mean_us=%lu p95_upper_us=%lu max_us=%lu\r\n",
      (unsigned long)(clock>>32),(unsigned long)clock,(unsigned long)seq,(unsigned long)sy.valid,(unsigned long)sy.count,
      (unsigned long)sy.rtt_us,(unsigned long)sy.bound_us,(long)sy.drift_ppm,(unsigned long)sy.age_us,
      (unsigned long)sy.rejected,(unsigned)input.timestamped,(unsigned)input.span_us,
      (unsigned long)fm,(unsigned long)fn,(unsigned long)iu,(unsigned long)ov,names[metric],
      (unsigned long)stat.n,(unsigned long)(stat.n?stat.min:UINT32_MAX),
      (unsigned long)(stat.n?stat.sum/stat.n:UINT32_MAX),(unsigned long)TimingStat_P95(&stat),
      (unsigned long)(stat.n?stat.max:UINT32_MAX));
    if(n>0 && n<(int)sizeof(line) && HAL_UART_Transmit_IT(&huart2,(uint8_t *)line,(uint16_t)n)==HAL_OK) {
        metric=(metric+1)%METRICS;previous=now;
    }
}
