#ifndef ESP_LINK_H
#define ESP_LINK_H

#include "main.h"
#include "djy_uart_protocol.h"
#include <stdbool.h>
#include <stdint.h>

/* A valid command must be refreshed by the ESP32.  Today's no-dial setup uses
 * this as the only TQV command source; expiry must therefore ramp to 0%. */
#define ESP_LINK_TIMEOUT_MS 500u

void EspLink_Init(UART_HandleTypeDef *uart);
bool EspLink_GetLiveTv(DjyUartLiveTv *command);
bool EspLink_IsFresh(void);
uint32_t EspLink_GetRxCount(void);

/* ★Live Expression용 — 함수 호출은 안 되고 변수만 읽히므로 전역으로 노출한다.
 *   rx  : 10Hz로 계속 증가해야 정상. 0이면 ESP32가 안 보내거나 TX/RX 반전.
 *   crc : 0 근처여야 정상. 같이 증가하면 배선 노이즈나 보드레이트 불일치. */
extern volatile uint32_t g_esp_rx_count;
extern volatile uint32_t g_esp_crc_err;
extern volatile uint32_t g_esp_byte_count;  /* 원시 수신 바이트 수 */
extern volatile uint32_t g_esp_rearm_fail;  /* 수신 재무장 실패 */
extern volatile uint32_t g_esp_uart_err;    /* UART 에러(ORE/FE/NE) */

/* 메인 루프에서 주기적으로 호출 — 수신이 죽어 있으면 되살린다. */
void EspLink_Watchdog(void);
uint32_t EspLink_GetCrcErrorCount(void);

#endif /* ESP_LINK_H */
