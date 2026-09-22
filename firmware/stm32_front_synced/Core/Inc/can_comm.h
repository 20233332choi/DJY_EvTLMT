#ifndef CAN_COMM_H
#define CAN_COMM_H
#include <stdint.h>
#include "board_time_sync.h"
#include "vehicle_clock.h"

void CAN_Init(void);
void CAN_SendSensorData(uint16_t sas_angle, uint16_t tps_raw, uint8_t flags);
void CAN_SendHeartbeat(uint8_t status);

#endif /* CAN_COMM_H */
