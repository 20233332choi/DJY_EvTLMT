#ifndef SD_LOGGER_H
#define SD_LOGGER_H
#include "torque_vectoring.h"
#include "safety_monitor.h"

void SD_Logger_Init(void);
void SD_Logger_Write(const TV_t *tv, SafeAction_t action);  /* 100Hz, ISR에서 호출 */
void SD_Logger_Flush(void);                                  /* 메인 루프에서 호출 */
void SD_Logger_Close(void);

#endif /* SD_LOGGER_H */
