#ifndef BOARD_TIME_SYNC_H
#define BOARD_TIME_SYNC_H
#include <stdint.h>
#include <stdbool.h>
#include "time_sync_model.h"
/* Dedicated standard CAN IDs, versioned independently from legacy 0x100. */
#define TS_ID_REQUEST 0x080u
#define TS_ID_RX_STAMP 0x081u
#define TS_ID_TX_STAMP 0x082u
#define TS_ID_SENSOR_TIME 0x101u
#define TS_SENSOR_MARKER 0xa1u
typedef struct {
    uint32_t valid, count, rtt_us, bound_us, age_us, rejected;
    int32_t drift_ppm;
} BoardTimeSyncStatus;
void BoardTimeSync_Init(bool master);
void BoardTimeSync_Poll(void);
bool BoardTimeSync_OnCan(uint32_t id,const uint8_t *d,uint8_t len,uint32_t received_us);
BoardTimeSyncStatus BoardTimeSync_Status(uint32_t now);
bool BoardTimeSync_Map(uint32_t front_us,uint32_t now,uint32_t *rear_us,uint32_t *bound);
bool BoardTimeSync_SendSensor(uint16_t sas,uint16_t tps,uint8_t flags,
                              uint32_t sampled_us,uint16_t span_us);
static inline uint16_t ts_u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static inline uint32_t ts_u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void ts_put16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}
static inline void ts_put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
#endif
