#ifndef TPS_SENSOR_H
#define TPS_SENSOR_H
#include <stdint.h>
#include <stdbool.h>

void     TPS_Init(void);
uint16_t TPS_ReadRaw(void);          /* 0~4095 */
bool     TPS_IsValid(uint16_t raw);  /* 단선/단락 진단 포함 */

#endif /* TPS_SENSOR_H */
