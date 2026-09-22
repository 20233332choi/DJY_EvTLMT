#ifndef FRONT_TIMING_H
#define FRONT_TIMING_H
#include <stdint.h>
typedef struct {
    uint32_t seq,started,period,sas_us,tps_us,acquire_us,enqueue_us,work_us;
    uint32_t esr;
    uint16_t sas,tps,flags;
} FrontTimingSample;
/* flags: SAS protocol error, TPS range error, SAS HAL error, TPS HAL error,
 * CAN pair enqueue failure (bits 0..4). Enqueue success is NOT a CAN ACK. */
void FrontTiming_Record(const FrontTimingSample *sample);
void FrontTiming_Poll(void);
#endif
