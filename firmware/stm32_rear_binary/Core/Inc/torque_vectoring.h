#ifndef TORQUE_VECTORING_H
#define TORQUE_VECTORING_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* 입력 */
    float    steering_angle_rad;  /* +면 좌회전 */
    float    tps_fraction;        /* 0..1 운전자 출력의지 */
    uint16_t rpm_left, rpm_right; /* 모터 RPM (raw) */
    float    imu_yaw_rate;        /* +면 좌회전(반시계) [rad/s] */

    /* 출력 */
    uint16_t dac_left, dac_right; /* DAC 코드 0..4095 */

    /* 로깅용 내부값 */
    float vehicle_speed;          /* [m/s] */
    float desired_yaw;            /* [rad/s] */
    float yaw_error;              /* [rad/s] */
    float delta_power;            /* [kW] 최종 적용된 차동전력 (TV든 ED든) */
    float power_left, power_right;/* [kW] */
    bool  tv_active;              /* 토크벡터링(폐루프) 개입 중 */
    bool  ed_active;              /* 전자식 디퍼런셜(개루프) 개입 중 — tv_active와 항상 배타 */
    float traction_scale;         /* 0..1, 횡G 불일치로 TV 개입을 줄인 비율 (1=정상) */
} TV_t;

void TV_Init(void);
void TV_Update(TV_t *tv);
void TV_Reset(void);            /* PID 적분·슬루·ED 필터 상태 전부 초기화 (STOP 진입 시) */

/* ★두 스위치는 배타적으로 동작한다 — TV가 실제로 개입 중이면 ED는 자동으로 꺼진다.
 * ED를 별도로 끄는 건 조향각 자체를 못 믿을 때(SAS 고장)뿐이다. */
void TV_SetTVEnabled(bool en);   /* 토글 스위치 + 안전판정 */
void TV_SetEDEnabled(bool en);   /* SAS 유효할 때만 true */

/* ★ESP32(핏 컴퓨터)가 내려보내는 최종 차동 강도. TV/ED 양쪽 출력에 곱해진다.
 * 게인은 공격적으로 두고 주행 중 강도만 조절하는 용도 — 재플래시 없이 수렴 가능. */
void  TV_SetStrength(float fraction);   /* 0..1 */
float TV_GetStrength(void);

//
void  TV_SetGearRatio(float r);
float TV_GetGearRatio(void);
//

#endif /* TORQUE_VECTORING_H */
