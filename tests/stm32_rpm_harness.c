/* PC HAL inputs; RPM implementation and calibration/filter headers are real. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define TIM2 ((void *)(uintptr_t)2)
#define TIM_CHANNEL_1 1u
#define TIM_CHANNEL_2 2u
#define HAL_TIM_ACTIVE_CHANNEL_1 1u
#define HAL_TIM_ACTIVE_CHANNEL_2 2u
typedef struct { void *Instance; uint32_t Channel; } TIM_HandleTypeDef;
TIM_HandleTypeDef htim2 = { TIM2, 0 };
static uint64_t sim_us;
static uint32_t irq_mask;
static uint32_t HAL_GetTick(void) { return (uint32_t)(sim_us / 1000u); }
static uint32_t __get_PRIMASK(void) { return irq_mask; }
static void __disable_irq(void) { irq_mask = 1; }
static void __set_PRIMASK(uint32_t mask) { irq_mask = mask; }
static int HAL_TIM_IC_Start_IT(TIM_HandleTypeDef *h, uint32_t ch) { (void)h; (void)ch; return 0; }
static uint32_t HAL_TIM_ReadCapturedValue(TIM_HandleTypeDef *h, uint32_t ch);
#include "rpm_sensor.c"
static uint32_t HAL_TIM_ReadCapturedValue(TIM_HandleTypeDef *h, uint32_t ch) {
    (void)h; (void)ch;
    return (uint32_t)(sim_us / RPM_TICK_US);
}

static void edge(uint64_t at, unsigned sides) {
    sim_us = at;
    if (sides & 1u) { htim2.Channel = HAL_TIM_ACTIVE_CHANNEL_1; HAL_TIM_IC_CaptureCallback(&htim2); }
    if (sides & 2u) { htim2.Channel = HAL_TIM_ACTIVE_CHANNEL_2; HAL_TIM_IC_CaptureCallback(&htim2); }
}

static void run(uint64_t duration, uint32_t left_period, uint32_t right_period) {
    uint64_t start = sim_us;
    for (uint64_t dt = RPM_TICK_US; dt <= duration; dt += RPM_TICK_US) {
        sim_us = start + dt;
        unsigned sides = (left_period && dt % left_period == 0 ? 1u : 0u) |
                         (right_period && dt % right_period == 0 ? 2u : 0u);
        if (sides) edge(sim_us, sides);
        if (dt % 10000u == 0) RPM_Update(); /* real 100Hz control interval */
    }
}

static void reset(uint64_t at) { sim_us = at; RPM_Init(); }
static void invalid_both(void) {
    CHECK(!RPM_IsFresh());
    CHECK(RPM_GetLeft() == 0 && RPM_GetRight() == 0);
    CHECK(RPM_GetLeftRaw() == 0 && RPM_GetRightRaw() == 0);
}

static void continuous_noise(void) {
    reset(0); run(2000000, 80, 80);
    printf("80us noise: L=%u R=%u fresh=%u captures=%lu rejected=%lu\n",
           RPM_GetLeft(), RPM_GetRight(), RPM_IsFresh(),
           (unsigned long)g_rpm_cap_count_r, (unsigned long)g_rpm_glitch_r);
    invalid_both();
}

#ifndef BASELINE
static void qualification(void) {
    reset(0); invalid_both(); CHECK(!RPM_HasRecentActivity());
    edge(100000, 3); RPM_Update(); invalid_both(); CHECK(RPM_HasRecentActivity());
    edge(200000, 3); RPM_Update(); invalid_both();
    edge(300000, 3); RPM_Update(); CHECK(RPM_IsFresh());
    CHECK(RPM_GetLeft() == 951 && RPM_GetRight() == 951);
}

static void independent_channels(void) {
    reset(0); run(2000000, 100000, 50000); CHECK(RPM_IsFresh());
    CHECK(RPM_GetLeft() == 951 && RPM_GetRight() == 1903);
    CHECK(g_rpm_glitch_l == 0 && g_rpm_glitch_r == 0);
    reset(0); run(1000000, 100000, 80);
    CHECK(RPM_IsLeftFresh() && !RPM_IsRightFresh() && !RPM_IsFresh());
    CHECK(RPM_GetLeft() == 951 && RPM_GetRight() == 0);
    CHECK(RPM_HasRecentActivity()); /* noisy zero is not permission for pit tuning */
    reset(0); run(1000000, 0, 100000);
    CHECK(!RPM_IsLeftFresh() && RPM_IsRightFresh() && !RPM_IsFresh());
}

static void threshold_and_bursts(void) {
    uint32_t below = ((RPM_MIN_PERIOD_US - 1u) / RPM_TICK_US) * RPM_TICK_US;
    uint32_t above = ((RPM_MIN_PERIOD_US + RPM_TICK_US - 1u) / RPM_TICK_US) * RPM_TICK_US;
    reset(0); run(2000000, below, below); invalid_both();
    reset(0); run(2000000, above, above);
    CHECK(RPM_IsFresh()); CHECK(RPM_GetLeft() > 7900 && RPM_GetLeft() <= 8000);
    reset(0);
    for (unsigned i = 1; i <= 20; ++i) {
        edge(i * 100000u, 3); RPM_Update(); invalid_both();
        edge(i * 100000u + 80u, 3); RPM_Update(); invalid_both();
    }
}

static void invalidation_and_recovery(void) {
    reset(0); run(1000000, 100000, 100000); CHECK(RPM_IsFresh());
    edge(1000080, 3); invalid_both(); /* no wait for the next control tick */
    RPM_Update(); invalid_both();
    edge(1100080, 3); RPM_Update(); invalid_both();
    edge(1200080, 3); RPM_Update(); CHECK(RPM_IsFresh()); CHECK(RPM_GetRight() == 951);
    reset(0); run(1000000, 12000, 12000);
    edge(sim_us + 10u, 3); edge(sim_us + 80u, 3); RPM_Update(); invalid_both();
    edge(sim_us + 100000u, 3); RPM_Update(); invalid_both();
    edge(sim_us + 100000u, 3); RPM_Update(); CHECK(RPM_GetLeft() == 951);
}

static void stop_and_long_gaps(void) {
    reset(0); run(1000000, 100000, 100000); CHECK(RPM_IsFresh());
    run(400000, 0, 0); CHECK(RPM_IsFresh());
    run(10000, 0, 0); invalid_both(); CHECK(!RPM_HasRecentActivity());
    run(300000, 100000, 100000); CHECK(RPM_IsFresh()); CHECK(RPM_GetRight() == 951);
    edge(sim_us + (uint64_t)UINT32_MAX + 120001u, 3); RPM_Update(); invalid_both();
    edge(sim_us + 100000u, 3); RPM_Update(); invalid_both();
    edge(sim_us + 100000u, 3); RPM_Update(); CHECK(RPM_GetRight() == 951);
}

static void timer_and_tick_wrap(void) {
    reset(((uint64_t)UINT32_MAX - 15000u) * RPM_TICK_US);
    run(1000000, 100000, 100000); CHECK(RPM_IsFresh()); CHECK(RPM_GetLeft() == 951);
    reset(((uint64_t)UINT32_MAX - 150u) * 1000u);
    run(1000000, 100000, 100000); CHECK(RPM_IsFresh()); CHECK(RPM_GetRight() == 951);
    g_rpm_cap_count_r = UINT32_MAX;
    edge(sim_us + 100000u, 2); RPM_Update();
    CHECK(g_rpm_cap_count_r == 0); CHECK(RPM_IsRightFresh());
}

static void reinit_and_interrupt_mask(void) {
    reset(0); run(1000000, 100000, 100000); irq_mask = 1;
    CHECK(RPM_IsFresh()); CHECK(irq_mask == 1);
    RPM_Update(); CHECK(irq_mask == 1);
    CHECK(RPM_HasRecentActivity()); CHECK(irq_mask == 1);
    RPM_Init(); CHECK(irq_mask == 1); invalid_both();
    CHECK(g_rpm_cap_count_l == 0 && g_rpm_glitch_r == 0); CHECK(!RPM_HasRecentActivity());
    irq_mask = 0;
    edge(sim_us + 100000u, 3); RPM_Update(); invalid_both();
}
#endif

int main(void) {
    continuous_noise();
#ifndef BASELINE
    qualification(); independent_channels(); threshold_and_bursts();
    invalidation_and_recovery(); stop_and_long_gaps(); timer_and_tick_wrap();
    reinit_and_interrupt_mask();
    puts("RPM capture regression scenarios PASS");
#endif
    return 0;
}
