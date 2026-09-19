#include "control_settings.h"

/* This module only selects and slews the requested strength.  The TV control
 * law remains in torque_vectoring.c and is deliberately untouched. */
static uint8_t s_tv_target;
static uint8_t s_tv_applied;
static uint8_t s_regen_requested;
static DjyDriveMode s_mode;
static uint8_t s_flags;
static bool s_fresh;
static uint8_t s_tv_limit;
static uint8_t s_configured_tv_limit;
static uint8_t s_regen_limit;
static uint8_t s_tv_ramp_units;
static uint8_t s_regen_ramp_units;
static uint8_t s_pit_flags;
static uint16_t s_tv_ramp_tenths;

static void update_tv_slew(void)
{
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

void ControlSettings_Init(void)
{
    s_tv_target = 0u;
    s_tv_applied = 0u;
    s_regen_requested = 0u;
    s_mode = DJY_MODE_RACE;
    s_flags = 0u;
    s_fresh = false;
    s_tv_limit = 100u;
    s_configured_tv_limit = 100u;
    s_regen_limit = 0u;
    s_tv_ramp_units = 20u;
    s_regen_ramp_units = 10u;
    s_pit_flags = DJY_PIT_FLAG_TV_PERMITTED;
    s_tv_ramp_tenths = 0u;
}

void ControlSettings_ApplyPitConfig(const DjyPitConfig *config)
{
    if (config == 0) return;
    s_configured_tv_limit = config->tv_limit_percent;
    s_tv_limit = s_configured_tv_limit;
    s_regen_limit = config->regen_limit_percent;
    s_tv_ramp_units = config->tv_ramp_10pct_s;
    s_regen_ramp_units = config->regen_ramp_5pct_s;
    s_pit_flags = config->flags;
}

void ControlSettings_Update10ms(const DjyDriverControl *received, bool fresh)
{
    s_fresh = fresh;
    s_tv_limit = s_configured_tv_limit;
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
        s_tv_target = 0u;
        s_regen_requested = 0u;
        s_flags = 0u;
    }
    update_tv_slew();
}

void ControlSettings_UpdateEsp10ms(uint8_t requested_percent,
                                   uint8_t limit_percent,
                                   bool enabled)
{
    bool tv_allowed = (s_pit_flags & DJY_PIT_FLAG_TV_PERMITTED) != 0u;
    s_fresh = true;
    /* Both values have already passed CRC, 0..100 and 5% quantization checks. */
    uint8_t command_limit = limit_percent <= 100u ? limit_percent : 100u;
    uint8_t effective_limit = command_limit < s_configured_tv_limit
                            ? command_limit : s_configured_tv_limit;
    s_tv_limit = effective_limit;
    s_tv_target = requested_percent < effective_limit ? requested_percent : effective_limit;
    if (!enabled || !tv_allowed) s_tv_target = 0u;
    s_regen_requested = 0u;
    s_mode = DJY_MODE_RACE;
    s_flags = enabled && tv_allowed ? DJY_CONTROL_FLAG_TV_ENABLE : 0u;
    update_tv_slew();
}

uint8_t ControlSettings_GetTvAppliedPercent(void) { return s_tv_applied; }
uint8_t ControlSettings_GetTvTargetPercent(void) { return s_tv_target; }
uint8_t ControlSettings_GetRegenRequestedPercent(void) { return s_regen_requested; }
uint8_t ControlSettings_GetRegenAppliedPercent(void) { return 0u; }
DjyDriveMode ControlSettings_GetMode(void) { return s_mode; }
uint8_t ControlSettings_GetFlags(void) { return s_flags; }
bool ControlSettings_IsFresh(void) { return s_fresh; }
uint8_t ControlSettings_GetTvLimitPercent(void) { return s_tv_limit; }
uint8_t ControlSettings_GetRegenLimitPercent(void) { return s_regen_limit; }
uint16_t ControlSettings_GetTvRampPercentPerSecond(void) { return (uint16_t)s_tv_ramp_units * 10u; }
uint16_t ControlSettings_GetRegenRampPercentPerSecond(void) { return (uint16_t)s_regen_ramp_units * 5u; }
uint8_t ControlSettings_GetPitFlags(void) { return s_pit_flags; }
