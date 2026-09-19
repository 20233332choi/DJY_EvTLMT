#ifndef DAC_OUTPUT_H
#define DAC_OUTPUT_H
#include <stdint.h>

/* 외부 MCP4822 (SPI3). 코드 단위 = 밀리볼트 (게인 2배, 1LSB=1mV). */
void DAC_Output_Init(void);
void DAC_SetLeftThrottle(uint16_t dac_code);   /* MCP4822 VOA — 좌측 */
void DAC_SetRightThrottle(uint16_t dac_code);  /* MCP4822 VOB — 우측 */
void DAC_SetSafeState(void);                   /* 양측 오프(0.90V = 코드 900) */

/* SPI 전송 실패 누적 카운터 — 실차에서 계속 오르면 배선/아이솔레이터 문제 */
extern volatile uint32_t g_dac_spi_fail;

/* ★TEMP 진단: 좌우에 동일한 코드를 단계별로 써 넣으며 USART2로 출력.
 * main.c의 DAC_SELFTEST_MODE=1일 때만 호출된다. 블로킹이며 리턴하지 않는다.
 * Board A / CAN / SD 없이 DAC 하드웨어만 단독 검증하는 용도. */
void DAC_SelfTest(void);

#endif /* DAC_OUTPUT_H */
