#include "rpm_sensor.h"
#include "board_config.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "filters.h"
#include "main.h"
#include "vehicle_clock.h"
#include "timing_diag.h"

/* CubeMX: TIM3 Input Capture Direct, CH1=PB4(좌)/CH2=PB5(우), Rising Edge,
 * PSC=839 (84MHz/840=100kHz → 10us/tick), ARR=0xFFFF(16비트), Filter=8, Pull-up.
 * 컨트롤러 30핀 커넥터 18번 핀(ALARM/SPD, 실측 피크 3.7~4.4V) → PB4/PB5 직결.
 * ★13번 핀(RXD)이 아니라 18번 핀이 진짜 SPD임 — board_config.h 주석 참고.
 * ★컨트롤러 소프트웨어 Display 탭 "SpecialFrame"이 0(순수 속도펄스)이어야
 * 정상 동작. 기본값 21(One-Line 모드)이면 디지털 통신 데이터가 나와서
 * 펄스로 오인식되어 엉뚱한 값이 찍힘.
 * ★"Speed Pulse" 설정이 0이면 SPD 출력 자체가 없다(핀이 뜬 상태로 노이즈만
 * 탄다). RPM_PULSES_PER_REV와 같은 값(11)으로 맞출 것.
 * ★GPIO 대체기능(AF2) 설정은 stm32f4xx_hal_msp.c의 HAL_TIM_IC_MspInit()에서
 * 정상 생성됨(main.c의 MX_GPIO_Init_2 USER CODE 블록은 중복 안전장치).
 *
 * ★PB4/PB5는 ADC가 없는 FT(5V 톨러런트) 핀이다. SPD 피크 4.4V가 절대최대정격
 * 안에 들어오므로, 저항을 깜빡해도 핀이 죽지 않는다. (PA0/PA1은 ADC 직결 TTa라
 * 3.6V가 상한이었고, 직렬저항 없이는 위험했다 — 그래서 여기로 되돌렸다.) */
extern TIM_HandleTypeDef htim3;

/* 채널별 상태 — 좌/우 로직이 갈라지지 않도록 하나의 구조체로 묶는다.
 * (예전엔 s_xxx_l / s_xxx_r 변수쌍을 ISR 안에서 두 번 복사해 썼다.) */
typedef struct {
    volatile uint32_t last_capture;   /* 마지막으로 "기준"으로 삼은 캡처값 [tick] */
    volatile uint32_t last_ms;        /* 그때의 HAL_GetTick() [ms] */
    volatile uint32_t period_us;      /* 마지막으로 검증 통과한 주기 [us], 0=무효 */
    volatile bool     first;          /* 아직 기준점이 없음 */
} RpmCh_t;

static RpmCh_t s_ch_l, s_ch_r;

/* ★TEMP 진단용: 캡처 인터럽트가 실제로 몇 번 들어왔는지(주기 계산과 무관) */
volatile uint32_t g_rpm_cap_count_l = 0;
volatile uint32_t g_rpm_cap_count_r = 0;
/* 노이즈로 판정해 버린 캡처 수 — 실차에서 이 값이 폭증하면 SPD 배선 문제다.
 * (RC 필터 추가 / 실드선 / 접지 분리를 검토할 것) */
volatile uint32_t g_rpm_glitch_l = 0;
volatile uint32_t g_rpm_glitch_r = 0;
/* 기준점 재동기 횟수 — 정지 후 재출발마다 채널당 1씩 오르는 게 정상이다.
 * 주행 중에 계속 오르면 펄스를 놓치고 있다는 뜻(배선/접촉 불량). */
volatile uint32_t g_rpm_desync_l = 0;
volatile uint32_t g_rpm_desync_r = 0;

/* 제어 틱마다 갱신되는 필터 출력 */
static uint16_t   s_rpm_l = 0, s_rpm_r = 0;
static Deglitch_t s_dg_l,  s_dg_r;
static LPF1_t     s_lpf_l, s_lpf_r;

void RPM_Init(void) {
    Deglitch_Reset(&s_dg_l);  Deglitch_Reset(&s_dg_r);
    LPF1_Reset(&s_lpf_l);     LPF1_Reset(&s_lpf_r);
    s_rpm_l = 0; s_rpm_r = 0;
    s_ch_l.first = true;  s_ch_l.period_us = 0;
    s_ch_r.first = true;  s_ch_r.period_us = 0;
    HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_1);
    HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_2);
}

/* 캡처된 주기의 판정 결과.
 * ★이 둘을 구분하는 게 핵심이다. 예전엔 하나의 "버림"으로 합쳐놨었는데,
 *   그 탓에 DESYNC 상태에서 영구 락업이 났다(아래 capture_event 주석 참고). */
typedef enum {
    PERIOD_OK,       /* 정상 — 주기 채택 */
    PERIOD_NOISE,    /* 물리적으로 불가능하게 빠름 → 노이즈 에지 */
    PERIOD_DESYNC    /* 기준점과 어긋남 → 랩어라운드 또는 펄스 누락 */
} PeriodVerdict_t;

/*  1) 너무 짧음 → 노이즈. RPM_MIN_PERIOD_US(≈11.9ms = 8000RPM 상당)보다 짧은
 *     간격은 물리적으로 불가능하다. 전기적 노이즈는 us 단위라 여기서 전부 걸린다.
 *  2) ★16비트 랩어라운드 → TIM3는 16비트라 10us/tick에서 655ms를 넘는 주기는
 *     카운터가 한 바퀴 돌아 "짧은 주기"로 앨리어싱된다. 그대로 두면 거의 정지
 *     상태에서 갑자기 고RPM이 찍히고, 그 거짓 차속으로 TV가 붙어버릴 수 있다.
 *     HAL_GetTick() 경과시간과 교차검증해서 크게 어긋나면 걸러낸다.
 *     (경과시간이 주기보다 훨씬 길다 = 한 바퀴 돌았거나 펄스를 놓쳤다) */
static PeriodVerdict_t classify_period(uint32_t period_us, uint32_t elapsed_ms) {
    if (period_us < RPM_MIN_PERIOD_US) return PERIOD_NOISE;
    if (elapsed_ms > (period_us / 1000u) + RPM_WRAP_TOL_MS) return PERIOD_DESYNC;
    return PERIOD_OK;
}

/* 한 채널분 캡처 처리.
 *
 * ★두 폐기 사유의 처리가 정반대여야 한다 — 예전에 둘 다 "기준점 유지"로
 *   묶어놨던 게 영구 락업의 원인이었다:
 *
 *     정지로 펄스가 655ms 이상 끊김
 *       → 재출발 첫 캡처에서 캡처값은 65536을 넘어 wrap(작은 값)인데
 *         HAL_GetTick 경과는 그대로 크다 → DESYNC 판정
 *       → (구버전) 기준점을 그대로 두고 return
 *       → 다음 캡처: 경과시간은 계속 커지는데 캡처차는 65536마다 되감김
 *       → 두 값이 다시 만날 수 없다 → **모든 캡처를 영원히 폐기**
 *     실제로 이 상태에서 17Hz(정상 주행)로 펄스가 들어와도 cap만 오르고
 *     glt가 같은 폭으로 따라 올라 RPM이 0에 붙어 있었다.
 *
 *   NOISE  : 기준점 유지 — 다음 진짜 에지가 "직전 진짜 에지" 기준으로 올바른
 *            주기를 만들게 한다. 기준점을 안 옮겨도 누적 주기가 계속 커지므로
 *            곧 MIN_PERIOD를 넘어 스스로 빠져나온다(락업 없음).
 *   DESYNC : 기준점을 **현재 에지로 재동기**하고 주기는 버린다. 다음 에지부터
 *            새 기준으로 정상 측정된다(한 펄스만 손해).
 */
static void capture_event(RpmCh_t *ch, uint32_t cap, uint32_t now,
                          volatile uint32_t *glitch, volatile uint32_t *desync) {
    if (!ch->first) {
        uint32_t period = ((cap - ch->last_capture) & 0xFFFFu) * RPM_TICK_US;
        switch (classify_period(period, now - ch->last_ms)) {
        case PERIOD_OK:
            ch->period_us = period;
            break;
        case PERIOD_NOISE:
            (*glitch)++;
            return;                 /* 기준점 유지 */
        case PERIOD_DESYNC:
        default:
            (*desync)++;
            ch->period_us = 0;      /* 낡은 주기로 가짜 RPM이 찍히지 않게 무효화 */
            break;                  /* 아래에서 기준점 재동기 */
        }
    }
    ch->first        = false;
    ch->last_capture = cap;
    ch->last_ms      = now;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance != TIM3) return;
    uint32_t now = HAL_GetTick();

    if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        uint32_t rejected=g_rpm_glitch_l;
        uint32_t edge=VehicleClock_Us32()-((__HAL_TIM_GET_COUNTER(htim)-HAL_TIM_ReadCapturedValue(htim,TIM_CHANNEL_1))&0xffffu)*RPM_TICK_US;
        g_rpm_cap_count_l++;
        capture_event(&s_ch_l, HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1),
                      now, &g_rpm_glitch_l, &g_rpm_desync_l);
        if(g_rpm_glitch_l==rejected)Timing_RpmEdge(0,edge,s_ch_l.period_us!=0);
    } else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_2) {
        uint32_t rejected=g_rpm_glitch_r;
        uint32_t edge=VehicleClock_Us32()-((__HAL_TIM_GET_COUNTER(htim)-HAL_TIM_ReadCapturedValue(htim,TIM_CHANNEL_2))&0xffffu)*RPM_TICK_US;
        g_rpm_cap_count_r++;
        capture_event(&s_ch_r, HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_2),
                      now, &g_rpm_glitch_r, &g_rpm_desync_r);
        if(g_rpm_glitch_r==rejected)Timing_RpmEdge(1,edge,s_ch_r.period_us!=0);
    }
}

static float period_to_rpm(uint32_t period_us) {
    if (period_us == 0) return 0.0f;
    /* freq[Hz] = 1e6/period_us, RPM = freq * 60 / PULSES_PER_REV * RPM_CAL_GAIN(실측보정) */
    float raw = 60000000.0f / ((float)period_us * (float)RPM_PULSES_PER_REV);
    return raw * RPM_CAL_GAIN;
}

/* 마지막 캡처 이후 흐른 시간이 저장된 주기보다 길면, 그 경과시간을 주기로 본다.
 * ★이게 없으면 감속/정지 시 RPM이 마지막 값에 그대로 얼어붙어 있다가
 * RPM_STALE_MS(400ms)가 지나는 순간 0으로 절벽처럼 떨어진다. 그 400ms 동안
 * "정지했는데 차속이 높다"고 착각해서 TV가 붙어버릴 수 있다.
 * 경과시간을 주기로 쓰면 RPM이 자연스럽게 우하향으로 감쇠한다. */
static float measure_rpm(const RpmCh_t *ch, uint32_t now_ms) {
    uint32_t period_us = ch->period_us;
    uint32_t age       = now_ms - ch->last_ms;
    if (age > RPM_STALE_MS || period_us == 0) return 0.0f;
    uint32_t effective = period_us;
    uint32_t age_us    = age * 1000u;
    if (age_us > effective) effective = age_us;
    return period_to_rpm(effective);
}

/* 100Hz 제어 틱에서 1회 호출.
 * 2차 방어: Deglitch(잔여 스파이크) → LPF fc=5Hz. 차속은 느린 물리량이라
 * 32ms 군지연이 제어 성능에 영향을 주지 않는다(차속은 TV 활성 판정과
 * 목표요레이트 스케일에만 쓰인다). */
void RPM_Update(void) {
    uint32_t now = HAL_GetTick();
    const float alpha = LPF1_Alpha(RPM_LPF_FC_HZ, CONTROL_DT);

    float l = measure_rpm(&s_ch_l, now);
    float r = measure_rpm(&s_ch_r, now);

    l = Deglitch_Update(&s_dg_l, l, RPM_MAX_STEP, DEGLITCH_MAX_REJECT);
    r = Deglitch_Update(&s_dg_r, r, RPM_MAX_STEP, DEGLITCH_MAX_REJECT);

    l = LPF1_Update(&s_lpf_l, l, alpha);
    r = LPF1_Update(&s_lpf_r, r, alpha);

    s_rpm_l = (uint16_t)CLAMP(l, 0.0f, 65535.0f);
    s_rpm_r = (uint16_t)CLAMP(r, 0.0f, 65535.0f);
}

uint16_t RPM_GetLeft(void)  { return s_rpm_l; }
uint16_t RPM_GetRight(void) { return s_rpm_r; }

/* 필터 전 원본 — 디버그 출력/캘리브레이션용 */
uint16_t RPM_GetLeftRaw(void)  { return (uint16_t)measure_rpm(&s_ch_l, HAL_GetTick()); }
uint16_t RPM_GetRightRaw(void) { return (uint16_t)measure_rpm(&s_ch_r, HAL_GetTick()); }

/* ★유효한 주기까지 함께 본다. 재동기 직후에는 last_ms가 방금 갱신돼 "신선"해
 * 보이지만 아직 주기가 없으므로, 그 상태를 유효하다고 보고하면 안 된다. */
bool RPM_IsFresh(void) {
    uint32_t now = HAL_GetTick();
    return ((now - s_ch_l.last_ms) <= RPM_STALE_MS) && (s_ch_l.period_us != 0) &&
           ((now - s_ch_r.last_ms) <= RPM_STALE_MS) && (s_ch_r.period_us != 0);
}

bool RPM_IsLeftFresh(void) {
    return HAL_GetTick() - s_ch_l.last_ms <= RPM_STALE_MS && s_ch_l.period_us != 0u;
}
bool RPM_IsRightFresh(void) {
    return HAL_GetTick() - s_ch_r.last_ms <= RPM_STALE_MS && s_ch_r.period_us != 0u;
}
