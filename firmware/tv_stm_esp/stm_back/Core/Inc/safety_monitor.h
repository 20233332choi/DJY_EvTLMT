#ifndef SAFETY_MONITOR_H
#define SAFETY_MONITOR_H
#include "common_types.h"

typedef enum {
    SAFE_ACTION_NONE = 0,      /* 정상 — TV 허용 */
    SAFE_ACTION_DISABLE_TV,    /* TV(폐루프)만 끔 → ED(개루프)가 차동을 이어받음 */
    SAFE_ACTION_DISABLE_DIFF,  /* TV+ED 모두 끔 → 좌우 완전 균등 분배 */
    SAFE_ACTION_STOP           /* 즉시 정지 (DAC off) */
} SafeAction_t;

void         Safety_Init(void);
void         Safety_Update(void);
FaultCode    Safety_GetFaultCode(void);
SafeAction_t Safety_GetAction(void);

#endif /* SAFETY_MONITOR_H */
