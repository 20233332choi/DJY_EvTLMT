#ifndef SENSOR_TIME_PAIR_H
#define SENSOR_TIME_PAIR_H
#include "board_time_sync.h"
typedef struct {
    uint8_t data[8];uint16_t data_seq,stamp_seq,span;
    uint32_t data_rx,stamp_rx,front_us;
    bool data_ok,stamp_ok;
} SensorTimePair;
static inline bool SensorTimePair_Push(SensorTimePair *p,uint32_t id,const uint8_t *d,uint8_t len,uint32_t now) {
    if(len!=8)return false;
    if(id==0x100u && d[7]==TS_SENSOR_MARKER) {
        memcpy(p->data,d,8);p->data_seq=ts_u16(d+5);p->data_rx=now;p->data_ok=true;
    } else if(id==TS_ID_SENSOR_TIME) {
        p->stamp_seq=ts_u16(d);p->front_us=ts_u32(d+2);p->span=ts_u16(d+6);p->stamp_rx=now;p->stamp_ok=true;
    } else return false;
    if(!p->data_ok || !p->stamp_ok || p->data_seq!=p->stamp_seq)return false;
    if(now-p->data_rx>5000u || now-p->stamp_rx>5000u){p->data_ok=p->stamp_ok=false;return false;}
    p->data_ok=p->stamp_ok=false;return true;
}
#endif
