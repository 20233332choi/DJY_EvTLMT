#ifndef VEHICLE_CLOCK_H
#define VEHICLE_CLOCK_H
#include <stdint.h>
void VehicleClock_Init(void);
uint64_t VehicleClock_NowUs(void);
uint32_t VehicleClock_Us32(void);
#endif
