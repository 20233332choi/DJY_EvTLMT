#ifndef RPM_SENSOR_H
#define RPM_SENSOR_H
#include <stdint.h>
#include <stdbool.h>

void     RPM_Init(void);
void     RPM_Update(void);     /* ★100Hz 제어 틱에서 1회 호출 — 필터 갱신 */
uint16_t RPM_GetLeft(void);    /* [RPM] 필터 적용값 */
uint16_t RPM_GetRight(void);   /* [RPM] 필터 적용값 */
uint16_t RPM_GetLeftRaw(void);  /* 필터 전 원본 — 디버그/캘리브레이션용 */
uint16_t RPM_GetRightRaw(void);
bool     RPM_IsFresh(void);    /* 좌/우 둘 다 최근에 펄스 수신했는지 */
bool     RPM_IsLeftFresh(void);
bool     RPM_IsRightFresh(void);

/* ★TEMP 진단용: 캡처 인터럽트 누적 횟수(주기 계산 성공 여부와 무관) */
extern volatile uint32_t g_rpm_cap_count_l;
extern volatile uint32_t g_rpm_cap_count_r;
/* 노이즈로 판정해 폐기한 캡처 수 — 실차에서 급증하면 SPD 배선을 의심할 것 */
extern volatile uint32_t g_rpm_glitch_l;
extern volatile uint32_t g_rpm_glitch_r;
/* 기준점 재동기 횟수 — 정지 후 재출발마다 채널당 1씩 오르는 게 정상.
 * 주행 중 계속 오르면 펄스 누락(배선/접촉 불량)이다. */
extern volatile uint32_t g_rpm_desync_l;
extern volatile uint32_t g_rpm_desync_r;

#endif /* RPM_SENSOR_H */
