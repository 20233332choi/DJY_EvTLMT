#ifndef TELEMETRY_UART_H
#define TELEMETRY_UART_H
#include "djy_uart_protocol.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define TELEMETRY_UART_COMMAND_TIMEOUT_MS 500u

/* PA9 TX -> ESP RX, PA10 RX <- ESP TX, 115200 8N1.  Text telemetry and the
 * fixed-size binary live-TV command use independent TX/RX state. */
HAL_StatusTypeDef TelemetryUart_Init(void);
HAL_StatusTypeDef TelemetryUart_Send(const uint8_t *data, uint16_t length);
bool TelemetryUart_GetLiveTv(DjyUartLiveTv *command);
bool TelemetryUart_IsCommandFresh(void);
uint32_t TelemetryUart_GetRxCount(void);
uint32_t TelemetryUart_GetCrcErrorCount(void);
#endif
