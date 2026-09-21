#include "control_settings.h"

/* 2 percent per 10 ms = 200 percent/s. A 0->100 command therefore takes
 * 0.5 s, preventing a step in the final left/right differential. */
static uint8_t s_tv_target;
static uint8_t s_tv_applied;
static uint8_t s_regen_requested;
static DjyDriveMode s_mode;
static uint8_t s_flags;
static bool s_fresh;
static uint8_t s_tv_limit;
static uint8_t s_regen_limit;
static uint8_t s_tv_ramp_units;
static uint8_t s_regen_ramp_units;
static uint8_t s_pit_flags;
static uint16_t s_tv_ramp_tenths;

static void update_tv_slew(void) {
    s_tv_ramp_tenths += s_tv_ramp_units;
    uint8_t step = (uint8_t)(s_tv_ramp_tenths / 10u);
    s_tv_ramp_tenths %= 10u;
    if (step == 0u) return;
    if (s_tv_applied < s_tv_target) {
        uint16_t next = (uint16_t)s_tv_applied + step;
        s_tv_applied = next > s_tv_target ? s_tv_target : (uint8_t)next;
    } else if (s_tv_applied > s_tv_target) {
        int16_t next = (int16_t)s_tv_applied - step;
        s_tv_applied = next < (int16_t)s_tv_target ? s_tv_target : (uint8_t)next;
    }
}

void ControlSettings_Init(void) {
    s_tv_target = 0u;
    s_tv_applied = 0u;
    s_regen_requested = 0u;
    s_mode = DJY_MODE_RACE;
    s_flags = 0u;
    s_fresh = false;
    s_tv_limit = 100u;
    s_regen_limit = 0u;
    s_tv_ramp_units = 20u;   /* 200 percent/s */
    s_regen_ramp_units = 10u;
    s_pit_flags = DJY_PIT_FLAG_TV_PERMITTED;
    s_tv_ramp_tenths = 0u;
}

void ControlSettings_ApplyPitConfig(const DjyPitConfig *config) {
    if (config == 0) return;
    s_tv_limit = config->tv_limit_percent;
    s_regen_limit = config->regen_limit_percent;
    s_tv_ramp_units = config->tv_ramp_10pct_s;
    s_regen_ramp_units = config->regen_ramp_5pct_s;
    s_pit_flags = config->flags;
}

void ControlSettings_Update10ms(const DjyDriverControl *received, bool fresh) {
    s_fresh = fresh;
    if (fresh && received != 0) {
        bool tv_allowed = (s_pit_flags & DJY_PIT_FLAG_TV_PERMITTED) != 0u;
        bool regen_allowed = (s_pit_flags & DJY_PIT_FLAG_REGEN_PERMITTED) != 0u;
        uint8_t tv_bounded = received->tv_percent < s_tv_limit ? received->tv_percent : s_tv_limit;
        uint8_t regen_bounded = received->regen_percent < s_regen_limit ? received->regen_percent : s_regen_limit;
        s_tv_target = tv_allowed && (received->flags & DJY_CONTROL_FLAG_TV_ENABLE)
                    ? tv_bounded : 0u;
        s_regen_requested = regen_allowed && (received->flags & DJY_CONTROL_FLAG_REGEN_ENABLE)
                          ? regen_bounded : 0u;
        s_mode = received->mode;
        s_flags = received->flags;
    } else {
        /* Loss of the settings link falls back to 50:50 drive. It does not
         * stop base propulsion; sensor/TPS safety remains responsible for it. */
        s_tv_target = 0u;
        s_regen_requested = 0u;
        s_flags = 0u;
    }

    update_tv_slew();
}

void ControlSettings_UpdateEsp10ms(uint8_t requested_percent,
                                   uint8_t limit_percent,
                                   bool enabled) {
    s_fresh = true;
    s_tv_limit = limit_percent <= 100u ? limit_percent : 100u;
    s_tv_target = requested_percent < s_tv_limit ? requested_percent : s_tv_limit;
    if (!enabled) s_tv_target = 0u;
    s_regen_requested = 0u;
    s_mode = DJY_MODE_RACE;
    s_flags = enabled ? DJY_CONTROL_FLAG_TV_ENABLE : 0u;
    update_tv_slew();
}

uint8_t ControlSettings_GetTvAppliedPercent(void) { return s_tv_applied; }
uint8_t ControlSettings_GetTvTargetPercent(void) { return s_tv_target; }
uint8_t ControlSettings_GetRegenRequestedPercent(void) { return s_regen_requested; }

/* Deliberately held at zero until the ND72680B regen command pin/protocol,
 * brake plausibility inputs, and BMS charge limits are implemented. */
uint8_t ControlSettings_GetRegenAppliedPercent(void) { return 0u; }

DjyDriveMode ControlSettings_GetMode(void) { return s_mode; }
uint8_t ControlSettings_GetFlags(void) { return s_flags; }
bool ControlSettings_IsFresh(void) { return s_fresh; }
uint8_t ControlSettings_GetTvLimitPercent(void) { return s_tv_limit; }
uint8_t ControlSettings_GetRegenLimitPercent(void) { return s_regen_limit; }
uint16_t ControlSettings_GetTvRampPercentPerSecond(void) { return (uint16_t)s_tv_ramp_units * 10u; }
uint16_t ControlSettings_GetRegenRampPercentPerSecond(void) { return (uint16_t)s_regen_ramp_units * 5u; }
uint8_t ControlSettings_GetPitFlags(void) { return s_pit_flags; }
