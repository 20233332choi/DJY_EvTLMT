#include "rpm_sensor.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "filters.h"
#include "main.h"

/* TIM2 CH1=PA0, CH2=PA1, 10us/tick. Pin/timer settings and the existing
 * pulses-per-revolution, calibration gain and output filters are unchanged.
 * This rejects implausibly fast captures; it cannot identify the electrical
 * source of noise or distinguish a stationary motor from a disconnected SPD. */
extern TIM_HandleTypeDef htim2;

typedef struct {
    uint32_t last_capture;
    uint32_t last_edge_ms;
    uint32_t period_us;
    uint32_t last_valid_ms;
    uint8_t clean_periods;
    bool seen;
} RpmCapture;

static volatile RpmCapture s_capture_l, s_capture_r;
volatile uint32_t g_rpm_cap_count_l, g_rpm_cap_count_r;
volatile uint32_t g_rpm_glitch_l, g_rpm_glitch_r;
static volatile uint16_t s_rpm_l, s_rpm_r;
static Deglitch_t s_dg_l, s_dg_r;
static LPF1_t s_lpf_l, s_lpf_r;

/* Debug/telemetry reads can be interrupted by capture. Preserve the caller's
 * interrupt mask; never re-enable interrupts inside an existing critical region. */
static RpmCapture snapshot(volatile RpmCapture *capture, uint32_t *now) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    RpmCapture value = *capture;
    *now = HAL_GetTick();
    __set_PRIMASK(mask);
    return value;
}

void RPM_Init(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    s_capture_l = (RpmCapture){0};
    s_capture_r = (RpmCapture){0};
    g_rpm_cap_count_l = g_rpm_cap_count_r = 0;
    g_rpm_glitch_l = g_rpm_glitch_r = 0;
    s_rpm_l = s_rpm_r = 0;
    Deglitch_Reset(&s_dg_l); Deglitch_Reset(&s_dg_r);
    LPF1_Reset(&s_lpf_l); LPF1_Reset(&s_lpf_r);
    __set_PRIMASK(mask);
    HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1);
    HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2);
}

static void capture_edge(volatile RpmCapture *s, uint32_t cap, uint32_t now,
                         volatile uint32_t *glitches) {
    uint32_t ticks = cap - s->last_capture; /* unsigned TIM2 wrap is intentional */
    uint32_t age = now - s->last_edge_ms;
    bool first = !s->seen;
    /* Always advance, including rejected edges. Otherwise continuous 80us
     * noise is accepted every ~11.9ms and aliases to roughly 8,000 RPM. */
    s->last_capture = cap;
    s->last_edge_ms = now;
    s->seen = true;

    /* Bound ticks BEFORE multiplying to avoid overflow after a long silence.
     * The first edge after startup/timeout is only a new timing baseline. */
    if (first || age > RPM_STALE_MS ||
        ticks > (RPM_STALE_MS * 1000u) / RPM_TICK_US) {
        s->clean_periods = 0;
        s->period_us = 0;
        return;
    }
    uint32_t period = ticks * RPM_TICK_US;
    if (period < RPM_MIN_PERIOD_US) {
        ++*glitches;
        s->clean_periods = 0;
        s->period_us = 0;
        return;
    }
    /* Two consecutive plausible intervals (three edges) are required after
     * startup, a timeout or a glitch. One quiet gap between bursts is not RPM. */
    if (s->clean_periods < 2u) ++s->clean_periods;
    if (s->clean_periods == 2u) {
        s->period_us = period;
        s->last_valid_ms = now;
    }
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != TIM2) return;
    uint32_t now = HAL_GetTick();
    if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        ++g_rpm_cap_count_l;
        capture_edge(&s_capture_l, HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1),
                     now, &g_rpm_glitch_l);
    } else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_2) {
        ++g_rpm_cap_count_r;
        capture_edge(&s_capture_r, HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_2),
                     now, &g_rpm_glitch_r);
    }
}

static bool is_fresh(const RpmCapture *s, uint32_t now) {
    return s->clean_periods == 2u && s->period_us != 0u &&
           (now - s->last_valid_ms) <= RPM_STALE_MS;
}

static float measure_rpm(const RpmCapture *s, uint32_t now) {
    if (!is_fresh(s, now)) return 0.0f;
    uint32_t effective = s->period_us;
    uint32_t age_us = (now - s->last_valid_ms) * 1000u;
    if (age_us > effective) effective = age_us;
    return (60000000.0f / ((float)effective * (float)RPM_PULSES_PER_REV)) * RPM_CAL_GAIN;
}

static uint16_t update_filter(volatile RpmCapture *capture, Deglitch_t *dg, LPF1_t *lpf) {
    uint32_t now;
    RpmCapture s = snapshot(capture, &now);
    if (!is_fresh(&s, now)) {
        /* Invalid is not a slow transition to a real zero-speed measurement.
         * Do not retain an old high RPM in the deglitch/IIR filter or on recovery. */
        Deglitch_Reset(dg);
        LPF1_Reset(lpf);
        return 0;
    }
    float rpm = Deglitch_Update(dg, measure_rpm(&s, now), RPM_MAX_STEP, DEGLITCH_MAX_REJECT);
    rpm = LPF1_Update(lpf, rpm, LPF1_Alpha(RPM_LPF_FC_HZ, CONTROL_DT));
    return (uint16_t)CLAMP(rpm, 0.0f, 65535.0f);
}

void RPM_Update(void) {
    s_rpm_l = update_filter(&s_capture_l, &s_dg_l, &s_lpf_l);
    s_rpm_r = update_filter(&s_capture_r, &s_dg_r, &s_lpf_r);
}

bool RPM_IsLeftFresh(void) {
    uint32_t now;
    RpmCapture s = snapshot(&s_capture_l, &now);
    return is_fresh(&s, now);
}

bool RPM_IsRightFresh(void) {
    uint32_t now;
    RpmCapture s = snapshot(&s_capture_r, &now);
    return is_fresh(&s, now);
}

bool RPM_IsFresh(void) { return RPM_IsLeftFresh() && RPM_IsRightFresh(); }
uint16_t RPM_GetLeft(void) { return RPM_IsLeftFresh() ? s_rpm_l : 0u; }
uint16_t RPM_GetRight(void) { return RPM_IsRightFresh() ? s_rpm_r : 0u; }

uint16_t RPM_GetLeftRaw(void) {
    uint32_t now;
    RpmCapture s = snapshot(&s_capture_l, &now);
    return (uint16_t)measure_rpm(&s, now);
}

uint16_t RPM_GetRightRaw(void) {
    uint32_t now;
    RpmCapture s = snapshot(&s_capture_r, &now);
    return (uint16_t)measure_rpm(&s, now);
}

bool RPM_HasRecentActivity(void) {
    uint32_t now;
    RpmCapture l = snapshot(&s_capture_l, &now);
    if (l.seen && (now - l.last_edge_ms) <= RPM_STALE_MS) return true;
    RpmCapture r = snapshot(&s_capture_r, &now);
    return r.seen && (now - r.last_edge_ms) <= RPM_STALE_MS;
}
