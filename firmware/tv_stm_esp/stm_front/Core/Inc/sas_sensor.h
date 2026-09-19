#ifndef SAS_SENSOR_H
#define SAS_SENSOR_H
#include <stdint.h>
#include <stdbool.h>

void     SAS_Init(void);
uint16_t SAS_ReadAngle(void);  /* 0~16383, 에러 시 마지막 유효값 유지 */
bool     SAS_HasError(void);   /* 직전 읽기 에러 여부 */

#endif /* SAS_SENSOR_H */
