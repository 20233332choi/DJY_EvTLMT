#ifndef FILTERS_H
#define FILTERS_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* =====================================================================
 *  실차 노이즈 대응 필터 프리미티브 (전부 inline — 100Hz ISR에서 호출)
 * ---------------------------------------------------------------------
 *  설계 원칙: "지연을 최소화한다"
 *
 *  - Deglitch  : 정상 신호에는 지연 0. 물리적으로 불가능한 점프만 걸러낸다.
 *                이동평균/미디안과 달리 평상시엔 입력을 그대로 통과시키므로
 *                제어 응답이 전혀 느려지지 않는다. 진짜 급변(스텝)일 때만
 *                max_reject 샘플만큼(=최대 20ms) 늦게 따라간다.
 *  - LPF1      : 1차 IIR. 군지연 ≈ 1/(2π·fc). fc=25Hz → 6.4ms, fc=15Hz → 10.6ms.
 *                미디안 필터(1샘플=10ms 고정 지연)보다 같은 지연 대비 감쇠가 좋다.
 *  - SlewLimit : 변화율 제한. 출력단에서 계단/튐을 막는 용도.
 *
 *  ★ 이동평균(moving average)은 일부러 안 썼다. N탭 이동평균의 군지연은
 *    (N-1)/2 샘플로 고정이라, 같은 노이즈 감쇠를 얻으려면 1차 IIR보다
 *    지연이 크다. 아날로그 노이즈는 소스에서(ADC 오버샘플링) 줄이는 게
 *    가장 싸다 — Board A의 TPS_ReadRaw()가 그 방식이다.
 * ===================================================================== */

/* ── 1차 IIR 저역통과 ─────────────────────────────────────────────── */
typedef struct { float y; bool init; } LPF1_t;

/* alpha = dt/(dt+RC), RC = 1/(2π·fc). 컴파일 타임 상수면 최적화로 사라진다. */
static inline float LPF1_Alpha(float fc_hz, float dt) {
    if (fc_hz <= 0.0f) return 1.0f;                 /* 필터 끔 */
    float rc = 1.0f / (2.0f * 3.14159265f * fc_hz);
    return dt / (dt + rc);
}
static inline void LPF1_Reset(LPF1_t *f) { f->y = 0.0f; f->init = false; }

static inline float LPF1_Update(LPF1_t *f, float x, float alpha) {
    if (!f->init) { f->y = x; f->init = true; return x; }  /* 첫 샘플은 그대로 */
    f->y += alpha * (x - f->y);
    return f->y;
}

/* ── 스파이크 제거기 (정상 구간 지연 0) ───────────────────────────────
 * 직전 확정값에서 max_step 이상 튀면 "노이즈"로 보고 무시하되, 연속으로
 * max_reject번 이상 같은 요구가 들어오면 진짜 변화로 인정하고 따라간다.
 * → 단발성 스파이크는 완전 제거, 진짜 급변은 max_reject 샘플만 늦게 반영. */
typedef struct { float y; uint8_t rej; bool init; } Deglitch_t;

static inline void Deglitch_Reset(Deglitch_t *d) { d->y = 0.0f; d->rej = 0; d->init = false; }

static inline float Deglitch_Update(Deglitch_t *d, float x,
                                    float max_step, uint8_t max_reject) {
    if (!d->init) { d->y = x; d->rej = 0; d->init = true; return x; }
    if (fabsf(x - d->y) > max_step && d->rej < max_reject) {
        d->rej++;
        return d->y;              /* 이번 샘플 버리고 직전값 유지 */
    }
    d->rej = 0;
    d->y   = x;
    return x;
}

/* ── 변화율 제한 ──────────────────────────────────────────────────── */
static inline float SlewLimit(float prev, float target, float max_delta) {
    float d = target - prev;
    if (d >  max_delta) return prev + max_delta;
    if (d < -max_delta) return prev - max_delta;
    return target;
}

#endif /* FILTERS_H */
