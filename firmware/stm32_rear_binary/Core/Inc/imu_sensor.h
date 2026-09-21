#ifndef IMU_SENSOR_H
#define IMU_SENSOR_H
#include <stdint.h>
#include <stdbool.h>
void IMU_OnRxIdle(void);

void  IMU_Init(void);
void  IMU_ProcessData(void);     /* 메인 루프에서 호출, DMA 링버퍼 파싱 */
void  IMU_Update(void);          /* ★100Hz 제어 틱에서 1회 호출 — 필터 갱신 */
void  IMU_Watchdog(void);        /* 메인 루프에서 호출 — DMA 정지 시 자동 복구 */

float IMU_GetYawRate(void);      /* [rad/s], +좌회전. 바이어스 보정 + 필터 적용값 */
float IMU_GetYawRateRaw(void);   /* 필터 전 원본(바이어스만 보정) — 디버그/튜닝용 */
float IMU_GetLateralAcc(void);   /* [m/s^2], 필터 적용값 */
bool  IMU_IsValid(void);         /* 타임아웃 + 이상치 검사 */
bool  IMU_IsTelemetryFresh(void);
float IMU_GetAccelerationX(void);
float IMU_GetAccelerationY(void);
float IMU_GetAccelerationZ(void);

/* 부팅 직후 차량이 완전히 정지한 상태에서 1회 호출 — duration_ms 동안
 * 블로킹하며 자이로 0점(바이어스)을 측정해 IMU_GetYawRate()에 자동 반영한다.
 * 반드시 IMU_Init() 이후, 제어 루프(TIM6) 시작 전에 호출할 것. */
void  IMU_Calibrate(uint32_t duration_ms);
bool  IMU_IsCalibrated(void);    /* 캘리브레이션 성공 여부 (샘플 부족/이상치면 false) */

/* ★TEMP 진단 카운터 — USART2 출력의 imu=gyro/bad/rsy 필드.
 *   셋 다 0      = UART4로 바이트가 전혀 안 들어옴 (전원/GND/TX-RX 반전)
 *   bad/rsy만 증가 = 바이트는 오는데 파싱 실패 (보드레이트/출력모드 불일치)
 *   gyro 증가    = 정상 */
extern volatile uint32_t g_imu_pkt_ok;
extern volatile uint32_t g_imu_pkt_bad;
extern volatile uint32_t g_imu_resync;
extern volatile uint32_t g_imu_gyro_ok;
extern volatile uint32_t g_imu_acc_ok;
extern volatile uint32_t g_imu_dma_restart;
extern volatile uint32_t g_imu_dma_fail;

#endif /* IMU_SENSOR_H */
