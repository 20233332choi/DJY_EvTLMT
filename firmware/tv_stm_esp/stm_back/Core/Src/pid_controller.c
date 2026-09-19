#include "pid_controller.h"
#include "common_types.h"

void PID_Init(PID_State *p, float Kp, float Ki, float Kd,
              float integral_max, float output_max) {
    p->Kp = Kp; p->Ki = Ki; p->Kd = Kd;
    p->integral_max = integral_max;
    p->output_max   = output_max;
    PID_Reset(p);
}

void PID_Reset(PID_State *p) {
    p->integral   = 0.0f;
    p->prev_error = 0.0f;
    p->first_run  = true;
}

float PID_Update(PID_State *p, float error, float dt) {
    float deriv = 0.0f;
    if (!p->first_run && dt > 0.0f) {
        deriv = (error - p->prev_error) / dt;
    }
    p->prev_error = error;
    p->first_run  = false;

    float p_term = p->Kp * error;
    float d_term = p->Kd * deriv;

    /* 임시 적분 갱신 */
    float integ = p->integral + error * dt;
    float i_term = p->Ki * integ;

    float out = p_term + i_term + d_term;

    /* anti-windup: 출력이 포화 & 에러가 포화를 더 키우는 방향이면 적분 정지 */
    bool saturated_hi = (out >  p->output_max) && (error > 0.0f);
    bool saturated_lo = (out < -p->output_max) && (error < 0.0f);
    if (!saturated_hi && !saturated_lo) {
        p->integral = CLAMP(integ, -p->integral_max, p->integral_max);
        i_term = p->Ki * p->integral;
        out = p_term + i_term + d_term;
    }
    /* (포화 시에는 직전 integral 유지) */

    return CLAMP(out, -p->output_max, p->output_max);
}
