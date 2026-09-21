#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* =====================================================================
 *  Board A  —  NUCLEO-F446RE  핀 배치도 (센서 입력 + CAN 송신 전용)
 * ---------------------------------------------------------------------
 *  기능          주변장치          핀      Nucleo 커넥터      AF
 *  ------------  ----------------  ------  -----------------  ----
 *  TPS (아날로그) ADC1_IN0          PA0     CN8-1  (A0)        analog
 *  SAS SCK       SPI1_SCK          PB3     CN10-31 (D3*)      AF5
 *  SAS MISO      SPI1_MISO         PB4     CN10-27 (D5*)      AF5
 *  SAS MOSI      SPI1_MOSI         PB5     CN10-29 (D4*)      AF5
 *  SAS CS        GPIO_Output(SW)   PB6     CN10-17 (D10)      -
 *  CAN RX        CAN1_RX           PA11    CN10-14            AF9
 *  CAN TX        CAN1_TX           PA12    CN10-12            AF9
 *  상태 LED      GPIO_Output       PA5     LD2 (온보드)       -
 *  디버그 UART   USART2 TX/RX      PA2/PA3 ST-Link VCP        AF7
 *  SWD           SWDIO/SWCLK       PA13/PA14                  -
 *
 *  ★ SPI1은 기본핀(PA5/PA6/PA7) 대신 PB3/PB4/PB5로 리맵했다.
 *    SPI1_SCK 기본핀 PA5가 온보드 LD2와 같은 핀이라 동시 사용이 불가능하다.
 *    부작용: PB3(JTDO/SWO) 점유 → SWO 트레이스 출력 불가. SWD 디버깅은 정상.
 *
 *  CubeMX 설정 요약
 *    ADC1  : IN0, 12-bit, 단일변환, 소프트웨어 트리거, 샘플타임 ≥ 84cycles
 *    SPI1  : Full-Duplex Master, Motorola, 16-bit, CPOL=Low CPHA=2Edge(Mode1),
 *            MSB first, Prescaler → ~1MHz, NSS = Software
 *    CAN1  : 500kbps, Prescaler/BitTiming은 APB1 클럭에 맞춰 산출
 *    TIM6  : 100Hz 인터럽트 (제어 주기)
 * ===================================================================== */

/* ── TPS (ADC) ─────────────────────────────────────────────────────── */
#define TPS_ADC_PORT    GPIOA
#define TPS_ADC_PIN     GPIO_PIN_0      /* PA0 = ADC1_IN0 (A0) */
#define TPS_ADC_CHANNEL ADC_CHANNEL_0

/* ── SAS : AS5147 (SPI1, 소프트웨어 CS) ────────────────────────────── */
#define SAS_SPI_SCK_PORT   GPIOB
#define SAS_SPI_SCK_PIN    GPIO_PIN_3   /* PB3  SPI1_SCK  (AF5) */
#define SAS_SPI_MISO_PORT  GPIOB
#define SAS_SPI_MISO_PIN   GPIO_PIN_4   /* PB4  SPI1_MISO (AF5) */
#define SAS_SPI_MOSI_PORT  GPIOB
#define SAS_SPI_MOSI_PIN   GPIO_PIN_5   /* PB5  SPI1_MOSI (AF5) */
#define SAS_CS_PORT        GPIOB
#define SAS_CS_PIN         GPIO_PIN_6   /* PB6  GPIO 출력, idle=High */

/* ── CAN1 ──────────────────────────────────────────────────────────── */
#define CAN_RX_PORT     GPIOA
#define CAN_RX_PIN      GPIO_PIN_11     /* PA11 CAN1_RX (AF9) */
#define CAN_TX_PORT     GPIOA
#define CAN_TX_PIN      GPIO_PIN_12     /* PA12 CAN1_TX (AF9) */

/* ── 상태 LED (NUCLEO LD2) ─────────────────────────────────────────── */
#define LED_PORT        GPIOA
#define LED_PIN         GPIO_PIN_5      /* PA5 — SPI1 리맵으로 확보됨 */

/* ── 토크벡터링 on/off 토글 스위치 ─────────────────────────────────
 * ★Board B(PC13)에서 이쪽으로 옮겨왔다. 스위치가 조종석에 있으니
 *  앞 보드에서 읽는 게 배선상 훨씬 짧다. 읽은 상태는 0x100 프레임의
 *  바이트4(SENSOR_FLAG_TV_SW)에 실어 Board B로 보낸다.
 * ★핀 번호와 커넥터 위치(CN7-23)가 Board B에서 쓰던 것과 동일하므로
 *  기존 스위치 하네스를 그대로 앞 보드에 옮겨 꽂으면 된다.
 * ★PC13은 온보드 B1 유저 버튼과 공유된다. CubeMX 생성 코드가 EXTI로
 *  잡아두지만, MX_GPIO_Init_2 USER CODE 블록에서 일반 입력(풀업)으로
 *  다시 설정한다(그 블록이 생성 코드 뒤에 실행된다). */
#define TV_SWITCH_PORT     GPIOC
#define TV_SWITCH_PIN      GPIO_PIN_13   /* PC13 — CN7-23 */
#define TV_SWITCH_ON_STATE GPIO_PIN_RESET /* 내부 풀업, 닫힘(GND)=ON */

/* 기계식 접점 채터링 + 배선 유도 노이즈 방지 — 100Hz 기준 50ms */
#define SWITCH_DEBOUNCE_TICKS 5u

/* ── TPS 유효 범위 (12bit, 3.3V 기준) ──────────────────────────────
 *  ★실측(2026-08-06, 벤치): 페달 idle~풀프레스 0.7V~2.5V → 868~3102
 *  ★★실차 장착 후 재실측(2026-09): idle 880~890, 풀프레스 2790~2820.
 *    페달 스토퍼가 센서 끝보다 먼저 닿아서 벤치값까지 안 올라간다.
 *    ★Board B의 vehicle_params.h와 반드시 같은 값을 쓸 것 — 한쪽만 바꾸면
 *     "앞 보드는 정상인데 뒤 보드는 폴트" 같은 모순이 생긴다.
 *  단선(→0V)·단락(→3.3V) 진단용 밴드. MARGIN은 끝단 오검출 방지 여유.
 *  밴드 = [685, 3020] */
#define TPS_ADC_MIN     885u
#define TPS_ADC_MAX     2820u
/* ★Board B의 vehicle_params.h와 같은 값(50→200)으로 맞췄다. Board B는 페달
 * 유격 대응으로 부팅 시 idle 위치를 ±150 범위에서 자동 학습하는데, 여기 마진이
 * 좁으면 "B는 정상 판정인데 A는 TPS_ERR 하트비트를 쏘는" 불일치가 생긴다.
 * 진짜 단선(→0 부근)/단락(→4095 부근)과는 여전히 한참 떨어져 있다. */
#define TPS_ADC_MARGIN  200u

#define CONTROL_FREQ_HZ 100u
#define HEARTBEAT_DIV   10u    /* 100Hz / 10 = 10Hz */

#endif /* BOARD_CONFIG_H */
