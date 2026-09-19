#ifndef IMU_SENSOR_H
#define IMU_SENSOR_H
#include <stdint.h>
#include <stdbool.h>

void  IMU_Init(void);
void  IMU_ProcessData(void);     /* 메인 루프에서 호출, DMA 링버퍼 파싱 */
void  IMU_Update(void);          /* ★100Hz 제어 틱에서 1회 호출 — 필터 갱신 */
void  IMU_Watchdog(void);        /* 메인 루프에서 호출 — DMA 정지 시 자동 복구 */

float IMU_GetYawRate(void);      /* [rad/s], +좌회전. 바이어스 보정 + 필터 적용값 */
float IMU_GetYawRateRaw(void);   /* 필터 전 원본(바이어스만 보정) — 디버그/튜닝용 */
float IMU_GetLateralAcc(void);   /* [m/s^2], 필터 적용값 */
float IMU_GetLongitudinalAcc(void); /* [m/s^2], 센서 종방향 원시값 */
float IMU_GetRawAx(void);        /* [m/s^2], WT 0x51 원시 X축 */
float IMU_GetRawAy(void);        /* [m/s^2], WT 0x51 원시 Y축 */
float IMU_GetRawAz(void);        /* [m/s^2], WT 0x51 원시 Z축 */
bool  IMU_IsValid(void);         /* 타임아웃 + 이상치 검사 */

/* 부팅 직후 차량이 완전히 정지한 상태에서 1회 호출 — duration_ms 동안
 * 블로킹하며 자이로 0점(바이어스)을 측정해 IMU_GetYawRate()에 자동 반영한다.
 * 반드시 IMU_Init() 이후, 제어 루프(TIM6) 시작 전에 호출할 것. */
void  IMU_Calibrate(uint32_t duration_ms);
bool  IMU_IsCalibrated(void);    /* 캘리브레이션 성공 여부 (샘플 부족/이상치면 false) */
uint32_t IMU_GetPacketOkCount(void);
uint32_t IMU_GetPacketBadCount(void);
uint32_t IMU_GetResyncCount(void);
uint32_t IMU_GetGyroPacketCount(void);
uint32_t IMU_GetAccPacketCount(void);

#endif /* IMU_SENSOR_H */
