#ifndef RPM_SENSOR_H
#define RPM_SENSOR_H
#include <stdint.h>
#include <stdbool.h>

void RPM_Init(void);
void RPM_Update(void); /* 100Hz, before Safety_Update() */
/* Invalid/no input returns zero; pair the value with freshness, not "stopped". */
uint16_t RPM_GetLeft(void);
uint16_t RPM_GetRight(void);
uint16_t RPM_GetLeftRaw(void);
uint16_t RPM_GetRightRaw(void);
bool RPM_IsLeftFresh(void);
bool RPM_IsRightFresh(void);
bool RPM_IsFresh(void); /* Both sides have qualified, non-stale periods. */
/* Includes rejected noise: invalid RPM must not authorize stationary tuning. */
bool RPM_HasRecentActivity(void);

extern volatile uint32_t g_rpm_cap_count_l, g_rpm_cap_count_r;
extern volatile uint32_t g_rpm_glitch_l, g_rpm_glitch_r;
#endif
