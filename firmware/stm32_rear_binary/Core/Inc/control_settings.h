#ifndef CONTROL_SETTINGS_H
#define CONTROL_SETTINGS_H

#include "djy_can_protocol.h"
#include <stdbool.h>
#include <stdint.h>

void ControlSettings_Init(void);
void ControlSettings_Update10ms(const DjyDriverControl *received, bool fresh);
void ControlSettings_UpdateEsp10ms(uint8_t requested_percent,
                                   uint8_t limit_percent,
                                   bool enabled);
void ControlSettings_ApplyPitConfig(const DjyPitConfig *config);
uint8_t ControlSettings_GetTvAppliedPercent(void);
uint8_t ControlSettings_GetTvTargetPercent(void);
uint8_t ControlSettings_GetRegenRequestedPercent(void);
uint8_t ControlSettings_GetRegenAppliedPercent(void);
DjyDriveMode ControlSettings_GetMode(void);
uint8_t ControlSettings_GetFlags(void);
bool ControlSettings_IsFresh(void);
uint8_t ControlSettings_GetTvLimitPercent(void);
uint8_t ControlSettings_GetRegenLimitPercent(void);
uint16_t ControlSettings_GetTvRampPercentPerSecond(void);
uint16_t ControlSettings_GetRegenRampPercentPerSecond(void);
uint8_t ControlSettings_GetPitFlags(void);

#endif /* CONTROL_SETTINGS_H */
