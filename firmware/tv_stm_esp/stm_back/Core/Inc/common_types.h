#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { STATE_INIT = 0, STATE_RUNNING, STATE_FAULT } SystemState;

typedef enum {
    FAULT_NONE = 0,
    FAULT_CAN_TIMEOUT,
    FAULT_TPS_RANGE,
    FAULT_IMU_INVALID,
    FAULT_RPM_TIMEOUT,
    FAULT_SAS_ERROR       /* Board A 하트비트의 SAS_ERR 비트 */
} FaultCode;

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))

#endif /* COMMON_TYPES_H */
