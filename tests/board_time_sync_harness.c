#include "time_sync_model.h"
#include "sensor_time_pair.h"
#include "timing_stats.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

static uint32_t remote(uint64_t rear,double speed) {
    return (uint32_t)(uint64_t)(rear*speed+1234567.0);
}
static void clocks(double speed,uint64_t start) {
    TS_Model m;TS_Reset(&m);uint32_t mapped,bound;
    CHECK(!TS_Map(&m,0,0,&mapped,&bound));
    uint64_t base=start;
    for(int i=0;i<40;i++) {
        base=start+(uint64_t)i*100000;
        unsigned forward=180+(i%3)*20,backward=260-(i%3)*20,service=300;
        CHECK(TS_Add(&m,(uint32_t)base,remote(base+forward,speed),remote(base+forward+service,speed),(uint32_t)(base+forward+service+backward)));
        if(i<7)CHECK(!m.ready);
    }
    CHECK(m.ready);CHECK(fabs(m.rate-1.0/speed)<.0001);
    uint32_t now=(uint32_t)(base+4000);
    CHECK(TS_Map(&m,remote(base+2000,speed),now,&mapped,&bound));
    CHECK(abs((int32_t)(mapped-(uint32_t)(base+2000)))<100);
    CHECK(bound<=1000);
    // Convert acquisition START and midpoint separately; do not add Front
    // microseconds directly to Rear delays when oscillators have skew.
    uint32_t mid=remote(base+2000,speed),span=390,acq_start;
    CHECK(TS_Map(&m,mid-span/2,now,&acq_start,&bound));
    CHECK(abs((int32_t)(mapped-acq_start)-(int32_t)lround(195.0/speed))<=1);
    CHECK(!TS_Map(&m,remote(base+2000,speed),(uint32_t)(base+600000),&mapped,&bound));
    unsigned count=m.count;
    CHECK(!TS_Add(&m,now,remote(base+4100,speed),remote(base+4200,speed),now+10000));
    CHECK(m.count==count && m.rejected==1);
    // Front reboot while Rear continues: invalidate rather than use old mapping.
    CHECK(TS_Add(&m,now+100000,100,400,now+100740));
    CHECK(!m.ready && m.count==1);
}
static void pairs(void) {
    SensorTimePair p={0};uint8_t data[8]={0},stamp[8]={0};
    ts_put16(data,6789);ts_put16(data+2,1000);data[4]=1;ts_put16(data+5,65535);data[7]=TS_SENSOR_MARKER;
    ts_put16(stamp,65535);ts_put32(stamp+2,0xfffffe00);ts_put16(stamp+6,80);
    CHECK(!SensorTimePair_Push(&p,0x100,data,8,0xfffffff0));
    CHECK(SensorTimePair_Push(&p,TS_ID_SENSOR_TIME,stamp,8,100));
    CHECK(p.front_us==0xfffffe00 && p.span==80 && ts_u16(p.data)==6789);
    CHECK(!SensorTimePair_Push(&p,TS_ID_SENSOR_TIME,stamp,8,200));
    ts_put16(data+5,0);CHECK(!SensorTimePair_Push(&p,0x100,data,8,300)); // unmatched seq
    ts_put16(stamp,0);CHECK(SensorTimePair_Push(&p,TS_ID_SENSOR_TIME,stamp,8,400));
    CHECK(!SensorTimePair_Push(&p,TS_ID_SENSOR_TIME,stamp,8,500));
    CHECK(!SensorTimePair_Push(&p,0x100,data,8,6000)); // pair too far apart
    CHECK(!SensorTimePair_Push(&p,0x100,data,5,6001)); // legacy not synchronized
    CHECK(!SensorTimePair_Push(&p,TS_ID_SENSOR_TIME,stamp,8,6100));
    CHECK(SensorTimePair_Push(&p,0x100,data,8,6200)); // reverse arrival order
}
int main(void) {
    clocks(1.0,1000000);clocks(1.02,1000000);clocks(.98,1000000);
    clocks(1.02,0xffff0000ull); // independent 32-bit wrap
    pairs();TimingStat s={0};CHECK(TimingStat_P95(&s)==UINT32_MAX);
    for(int i=0;i<95;i++)TimingStat_Add(&s,300);
    for(int i=0;i<5;i++)TimingStat_Add(&s,5001);
    CHECK(s.n==100 && s.min==300 && s.max==5001 && TimingStat_P95(&s)==500);
    puts("clock, wrap, expiry, skew, reboot, pairing and histograms PASS");
}
