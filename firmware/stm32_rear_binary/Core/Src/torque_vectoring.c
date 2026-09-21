#include "torque_vectoring.h"
#include "pid_controller.h"
#include "electronic_diff.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "filters.h"
#include "imu_sensor.h"
#include <math.h>

//
static float s_gear_ratio = GEAR_RATIO_DEFAULT;

void  TV_SetGearRatio(float r) { if (r > 0.1f) s_gear_ratio = r; }
float TV_GetGearRatio(void)    { return s_gear_ratio; }
//

static PID_State s_pid;
static bool      s_tv_enabled = false;
static bool      s_ed_enabled = true;    /* SAS가 멀쩡한 한 항상 켜둔다 */
static float     s_dp_prev    = 0.0f;    /* 슬루 리미터 상태 */

/* ★ESP32(핏 컴퓨터)에서 내려오는 최종 차동 강도 0..1. TV와 ED 양쪽에 곱해진다.
 * 게인을 공격적으로 잡아두고 주행 중에 강도만 내려서 쓸 수 있게 하는 손잡이다.
 * ★초기값은 TV_STRENGTH_NO_ESP — ESP 링크가 없는 구성에서 TV가 조용히 죽어버리는
 *  걸 막는다. 팀원 원본은 0.0(= ESP 명령이 있어야만 개입)이었는데, 지금은 ESP32
 *  없이도 주행하므로 기본값을 1.0으로 두고 vehicle_params.h에서 바꾸게 했다. */
static float     s_strength   = TV_STRENGTH_NO_ESP;

void TV_Init(void) {
    PID_Init(&s_pid, PID_KP, PID_KI, PID_KD, PID_INTEGRAL_MAX, PID_OUTPUT_MAX);
    ED_Init();
    s_tv_enabled = false;
    s_ed_enabled = true;
    s_dp_prev    = 0.0f;
    s_strength   = TV_STRENGTH_NO_ESP;
}

void  TV_SetStrength(float fraction) { s_strength = CLAMP(fraction, 0.0f, 1.0f); }
float TV_GetStrength(void)           { return s_strength; }

/* ★STOP 진입 시 반드시 호출할 것. 예전엔 STOP이면 TV_Update() 자체를 안 불러서
 * PID 적분값이 그대로 남아 있었고, 폴트가 풀리는 순간 쌓여있던 적분항이 한꺼번에
 * 튀어나왔다(적분 windup). 슬루 상태와 ED 필터도 같이 0으로 되돌린다. */
void TV_Reset(void) {
    PID_Reset(&s_pid);
    ED_Reset();
    s_dp_prev = 0.0f;
}

void TV_SetTVEnabled(bool en) { s_tv_enabled = en; }
void TV_SetEDEnabled(bool en) { s_ed_enabled = en; }

/* 모터 RPM(TIM3 Input Capture 실측) → 차속[m/s]. 체인 감속비 반영. */
static float compute_speed(uint16_t rpm_l, uint16_t rpm_r) {
    float wheel_rpm = 0.5f * ((float)rpm_l + (float)rpm_r) / s_gear_ratio;
    return wheel_rpm * (2.0f * (float)M_PI * TIRE_RADIUS) / 60.0f;
}

/* Ackermann 목표 yaw rate — 언더스티어 그래디언트(Kus) 반영, 횡가속 한계로 클램프.
 * Kus=0(기본값)이면 분모가 WHEELBASE만 남아 기존 순수 기구학 모델과 동일하다. */
static float desired_yaw_rate(float v, float delta) {
    if (v < MIN_SPEED_FOR_TV) return 0.0f;
    float psi = v * tanf(delta) / (WHEELBASE + UNDERSTEER_GRADIENT * v * v);
    float psi_max = MAX_LATERAL_ACCEL / fmaxf(v, 0.1f);  /* a_lat = v*psi */
    return CLAMP(psi, -psi_max, psi_max);
}

/* 예측 횡가속도(v×실제요레이트) vs IMU 실측 횡가속도 불일치로 트랙션 상실을
 * 감지 — 자전거모델은 타이어가 그립하고 있다는 전제라, 미끄러지는 중이면
 * 이 둘이 크게 어긋난다. 불일치가 크면 TV 개입(dP)을 줄여서 상황을 악화시키지
 * 않게 한다. 반환값 1=정상(그대로), 0=완전히 개입 중단. */
static float traction_scale(float v, float yaw_rate_meas, float lat_acc_meas) {
    float predicted = v * yaw_rate_meas;
    float mismatch  = fabsf(lat_acc_meas - predicted);
    if (mismatch <= LAT_ACC_MISMATCH_OK)  return 1.0f;
    if (mismatch >= LAT_ACC_MISMATCH_MAX) return 0.0f;
    return (LAT_ACC_MISMATCH_MAX - mismatch) / (LAT_ACC_MISMATCH_MAX - LAT_ACC_MISMATCH_OK);
}

/* 속도 의존 ΔP 상한 — 좌우 구동력 차 ΔF = ΔP/v 가 타이어 한계를 넘지 않게.
 * 저속일수록 같은 kW가 훨씬 큰 힘이 되므로 이 제한이 없으면 저속 코너에서
 * 안쪽 바퀴가 완전히 죽고 바깥쪽이 휠스핀한다. (vehicle_params.h 주석 참고) */
/* ★세 번째 제한: 운전자 요구량에 비례한 상한.
 *  앞의 두 제한(kW 천장, ΔF)은 **스로틀과 무관**해서, 부분 스로틀에서
 *  ΔP가 base를 넘어버리면 안쪽 바퀴가 0으로 깎인다:
 *      30% 스로틀 → base 1.425kW, ΔP 4.5 → 안쪽 0 / 바깥 2.85 (한쪽 구동)
 *  합계는 보존되지만 토크가 한 바퀴에 몰려 헛돌기 쉽고, 운전자는
 *  "TV 켜면 출력이 낮다"고 느낀다(2026-09 실주행 피드백).
 *  요구량에 비례해 묶으면 안쪽 바퀴가 항상 (1−FRAC)/2 만큼은 살아있다. */
static float delta_power_limit(float v, float p_demand) {
    float by_force  = DELTA_FORCE_MAX_N * v * 0.001f;   /* N·m/s → kW */
    float by_demand = TV_DELTA_DEMAND_FRAC * p_demand;
    return fminf(fminf(DELTA_POWER_MAX_KW, by_force), by_demand);
}

/* kW → DAC 코드 (컨트롤러 전압 매핑의 정확한 역함수)
 * ★외부 MCP4822는 게인 2배에서 1LSB = 1mV라 "전압×1000"이 곧 코드다.
 *   내부 DAC 시절의 (v/3.3*4095) 환산이 사라져 계산이 단순해졌다. */
static uint16_t power_to_dac(float P_kW) {
    float v;
    if (P_kW <= P_OFF_EPS_KW) v = V_THROTTLE_OFF;          /* 0.90V → 0kW 보장 */
    else                      v = V_CTRL_0KW + P_kW * V_PER_KW;

    /* 전 구간 하나의 선형식만 쓰고 상단은 V_DAC_MAX_V(3.20V)로 자른다.
     * 정상 동작에서 최대는 7kW→3.00V라 이 클램프에 안 걸린다. 스케일링 버그로
     * 4V가 컨트롤러 스로틀에 나가는 사고를 막는 안전망이다. */
    v = CLAMP(v, 0.0f, V_DAC_MAX_V);
    float code = v * DAC_CODE_PER_V;
    return (uint16_t)CLAMP(code, 0.0f, (float)DAC_RESOLUTION);
}

void TV_Update(TV_t *tv) {
    float v     = compute_speed(tv->rpm_left, tv->rpm_right);
    float delta = tv->steering_angle_rad;
    tv->vehicle_speed = v;

    /* 운전자 요구 총전력과 기준 분배 (제로섬의 기준점) */
    float P_demand = CLAMP(tv->tps_fraction, 0.0f, 1.0f) * P_SUM_MAX_KW;
    float base     = 0.5f * P_demand;

    /* ── TV / ED 배타 중재 ──────────────────────────────────────────
     * TV가 실제로 개입 가능한 조건이면 TV, 아니면 ED. 절대 동시에 켜지지 않는다.
     * ED가 커버하는 구간:
     *   - 토글 스위치 off
     *   - IMU/RPM 폴트 (SAFE_ACTION_DISABLE_TV) → ED는 IMU를 안 쓰므로 생존
     *   - v < MIN_SPEED_FOR_TV (TV의 목표요레이트 모델이 성립 안 하는 저속)
     * P_demand≈0이면 둘 다 자연히 ΔP=0이 된다. */
    bool tv_active = s_tv_enabled && (v >= MIN_SPEED_FOR_TV) && (P_demand > P_OFF_EPS_KW);
    bool ed_active = s_ed_enabled && !tv_active && (P_demand > P_OFF_EPS_KW);

    float dP_raw = 0.0f;

    if (tv_active) {
        float psi_ref = desired_yaw_rate(v, delta);
        float err     = psi_ref - tv->imu_yaw_rate;
        dP_raw        = PID_Update(&s_pid, err, CONTROL_DT);  /* [kW], 제로섬 차동 */

        /* 횡G 불일치(트랙션 상실) 감지되면 개입을 줄인다 */
        float tscale = traction_scale(v, tv->imu_yaw_rate,
                                      IMU_GetLateralAcc());   /* 부호는 imu_sensor.c에서 이미 적용 */
        dP_raw *= tscale;

        /* 저속 과대개입 방지 (ΔF 한계) */
        float lim = delta_power_limit(v, P_demand);
        dP_raw = CLAMP(dP_raw, -lim, lim);

        tv->desired_yaw    = psi_ref;
        tv->yaw_error      = err;
        tv->traction_scale = tscale;

        /* TV 개입 중에도 ED 필터를 0으로 계속 굴려준다 → ED로 되돌아오는
         * 순간 필터가 이전 값에서 튀어나오지 않고 0에서 부드럽게 올라온다. */
        (void)ED_ComputeDeltaPower(delta, 0.0f, v);
    } else {
        PID_Reset(&s_pid);
        tv->desired_yaw    = 0.0f;
        tv->yaw_error      = 0.0f;
        tv->traction_scale = 1.0f;

        /* ED: 개루프 애커만 차동. IMU 전혀 사용 안 함. */
        dP_raw = ED_ComputeDeltaPower(delta, ed_active ? P_demand : 0.0f, v);
    }

    /* ★ESP32 강도 스케일 — TV/ED 어느 쪽이 만든 값이든 여기서 한 번에 곱한다.
     * 슬루 리미터보다 앞에 둬야 강도를 내릴 때도 변화율 제한이 걸린다. */
    dP_raw *= s_strength;

    /* 최종 변화율 제한 — TV↔ED 전환 시 계단을 없애고(둘을 동시에 켜서 블렌딩
     * 하지 않아도 매끄럽게 넘어간다), 남은 센서 노이즈가 출력으로 새는 것도 막는다. */
    float dP = SlewLimit(s_dp_prev, dP_raw, DELTA_POWER_SLEW_KW_S * CONTROL_DT);
    s_dp_prev = dP;

    tv->delta_power = dP;
    tv->tv_active   = tv_active;
    tv->ed_active   = ed_active;

    /* 제로섬 분배: dP>0 → 우측↑/좌측↓ → 좌회전 촉진 */
    float PL = base - 0.5f * dP;
    float PR = base + 0.5f * dP;

    /* 차동보존 클램프: 바깥이 모터 한계를 넘으면, 초과분을 안쪽에서
     * 추가로 빼서 차동(ΔP)을 유지한다. (기존: 바깥만 잘라 차동 손실) */
    if (PR > MOTOR_MAX_KW) { float ex = PR - MOTOR_MAX_KW; PR = MOTOR_MAX_KW; PL -= ex; }
    if (PL > MOTOR_MAX_KW) { float ex = PL - MOTOR_MAX_KW; PL = MOTOR_MAX_KW; PR -= ex; }

    /* ★음수(하한) 클램프도 위와 동일하게 제로섬 보존해야 한다 — 안 그러면
     * 한쪽이 0으로 잘리는 만큼 반대쪽에서도 빼주지 않아 합계가 P_demand를
     * 초과해서(운전자 요청보다 더 많은 힘이 나가는) 안전 문제가 생긴다. */
    if (PR < 0.0f) { float ex = -PR; PR = 0.0f; PL -= ex; }
    if (PL < 0.0f) { float ex = -PL; PL = 0.0f; PR -= ex; }

    /* 최종 안전망 (위 로직으로 이미 [0, MOTOR_MAX_KW] 안에 있어야 정상) */
    PL = CLAMP(PL, 0.0f, MOTOR_MAX_KW);
    PR = CLAMP(PR, 0.0f, MOTOR_MAX_KW);

    /* 최종 안전 포화: 합이 예산 초과면 비례 축소 — 합 ≤ P_SUM_MAX 수학적 보장 */
    float sum = PL + PR;
    if (sum > P_SUM_MAX_KW && sum > 0.0f) {
        float k = P_SUM_MAX_KW / sum;
        PL *= k; PR *= k;
    }

    tv->power_left  = PL;
    tv->power_right = PR;
    tv->dac_left    = power_to_dac(PL);
    tv->dac_right   = power_to_dac(PR);
}
