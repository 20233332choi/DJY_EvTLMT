#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* =====================================================================
 *  Board B  —  NUCLEO-F446RE  핀 배치도 (제어 + 출력 + 로깅)
 * ---------------------------------------------------------------------
 *  ▣ CN7 — 스로틀 출력(절연 SPI) + IMU
 *  기능           주변장치          핀      커넥터 위치       AF
 *  -------------  ---------------- ------  ----------------  ----
 *  DAC SCK        SPI3_SCK         PC10    CN7-1             AF6
 *  DAC CS         GPIO_Output(SW)  PC11    CN7-2             -
 *  DAC MOSI       SPI3_MOSI        PC12    CN7-3             AF6
 *                 (GND)                    CN7-8
 *  TV 토글 스위치 GPIO_Input PU     PC13    CN7-23 / 온보드B1 -
 *  IMU TX(→센서)  UART4_TX         PA0     CN7-28  (A0)      AF8
 *  IMU RX(←센서)  UART4_RX         PA1     CN7-30  (A1)      AF8
 *  상태 LED       GPIO_Output      PB0     CN7-34  (A3)      -
 *
 *  ▣ CN10 — RPM 입력 + 통신 + 로깅
 *  기능           주변장치          핀      커넥터 위치       AF
 *  -------------  ---------------- ------  ----------------  ----
 *  CAN TX         CAN1_TX          PA12    CN10-12           AF9
 *  CAN RX         CAN1_RX          PA11    CN10-14           AF9
 *  SD CS          GPIO_Output(SW)  PB1     CN10-24           -
 *  SD MOSI        SPI2_MOSI        PB15    CN10-26           AF5
 *  좌 RPM 펄스    TIM3_CH1(IC)     PB4     CN10-27  (D5)     AF2
 *  SD MISO        SPI2_MISO        PB14    CN10-28           AF5
 *  우 RPM 펄스    TIM3_CH2(IC)     PB5     CN10-29  (D4)     AF2
 *  SD SCK         SPI2_SCK         PB13    CN10-30           AF5
 *  SWO(트레이스)  SYS_JTDO-SWO     PB3     CN10-31           -
 *  디버그 UART    USART2 TX/RX     PA2/PA3 CN10-35/37 (VCP)  AF7
 *
 *  ★★ PA4 / PA5 는 사용하지 않는다
 *    내부 DAC을 버리고 외부 MCP4822로 갔기 때문이다. 두 핀이 비어 있으므로
 *    내부 DAC 채널이 손상된 보드도 전부 정상적으로 재사용할 수 있다.
 *    (vehicle_params.h의 DAC_USE_INTERNAL=1로 두면 임시 진단용으로만 되살아난다)
 *
 *  ★ 2026-08 재배치 이력 — 왜 이 배치인가
 *   1) SPD(RPM) → PB4/PB5 : 이 두 핀은 ADC가 없어 **FT(5V 톨러런트)** 다.
 *      컨트롤러 SPD 실측 피크가 3.7~4.4V인데, PA0/PA1 같은 TTa(3.3V) 핀은
 *      절대최대정격 3.6V를 넘겨 직렬저항 없이는 핀이 죽는다. FT 핀으로 두면
 *      저항을 깜빡해도 살아남는다(그래도 노이즈용 RC는 넣을 것).
 *   2) IMU → PA0/PA1 (UART4) : 3.3V 로직 신호라 TTa로 충분하고, CN7-28/30
 *      인접 2핀이라 하네스가 깔끔하다.
 *   3) DAC SPI → PC10/11/12 : CN7-1/2/3 연속 3핀. 절연 모듈로 한 다발.
 *   4) PB3가 풀려서 SWO(트레이스 출력)가 복원됐다.
 *
 *  ★ RPM이 SD SPI(CN10-26/28/30)와 커넥터에서 맞물린다
 *    12MHz SPI 클럭 옆을 SPD 입력이 지나므로, SPD 라인의 RC 저역통과
 *    (직렬 1kΩ + 핀-GND 10nF)를 반드시 넣을 것. 5V 톨러런스와는 별개로
 *    노이즈 대책으로 필요하다.
 *
 *  ★ USART2(PA2/PA3)는 ST-Link 가상COM에 배선돼 있어 옮기면 PuTTY를 못 쓴다.
 *  ★ Board B는 TPS/SAS를 직접 읽지 않는다. 전부 CAN(0x100)으로 수신한다.
 *  ★ RPM은 CAN이 아니라 컨트롤러(ND72680B) 30핀 커넥터의 **18번 핀**
 *    (ALARM/SPD 겸용)을 TIM3 Input Capture로 직접 측정한다.
 *    ★★ 13번 핀은 SPD가 아니라 RXD(시리얼)였음 — 착각해서 한참 헤맸음.
 *    12관/NS 시리즈 컨트롤러 기준: 13번=RXD, 18번=ALARM/SPD, 9번=SPA(아날로그,
 *    최대 60V — 절대 직결 금지).
 *    ★★ 소프트웨어 Display 탭의 "SpecialFrame"을 반드시 0으로 설정할 것
 *    (0=순수 속도펄스 모드. 기본값 21은 One-Line 디지털 프로토콜 모드라
 *    펄스로 오인식되어 엉뚱한 값이 나옴 — 실제로 겪었던 문제).
 *    Fardriver 소프트웨어의 "Speed Pulse"(1~16) 설정값과
 *    vehicle_params.h의 RPM_PULSES_PER_REV를 반드시 일치시킬 것.
 *
 *  CubeMX 설정 요약
 *    SPI3   : Transmit Only Master, 8-bit, CPOL=Low CPHA=1Edge(Mode0),
 *             프리스케일러 32(≈1.3MHz), NSS = Software (CS는 PC11 GPIO)
 *    UART4  : 115200 8N1, RX에 DMA1 Stream2 Ch4 — Circular 모드 필수
 *    SPI2   : Full-Duplex Master, 8-bit, CPOL=Low CPHA=1Edge(Mode0),
 *             초기화 시 <400kHz → 마운트 후 ~12MHz, NSS = Software
 *    CAN1   : 500kbps (Board A와 동일), RX FIFO0 인터럽트 활성
 *    TIM6   : 100Hz 인터럽트 (제어 루프)
 *    TIM3   : Input Capture Direct, CH1/CH2, Rising Edge, Filter=8, Pull-up,
 *             PSC=839(84MHz/840=100kHz→10us/tick), ARR=0xFFFF(16비트),
 *             NVIC TIM3 global interrupt Enable
 *             ★16비트라 655ms를 넘는 주기는 앨리어싱된다 —
 *               rpm_sensor.c의 period_plausible()이 교차검증으로 걸러낸다.
 * ===================================================================== */

/* ── DAC : 외부 MCP4822 (SPI3) — 모터 컨트롤러 스로틀 지령 ──────────
 * ★2026-08 변경: STM32 내부 DAC(PA4/PA5) → 외부 MCP4822로 교체.
 *
 * 이유:
 *  1) PA4/PA5는 TTa(3.3V 톨러런트) 핀이라 컨트롤러 신호 환경을 못 버텼다.
 *     오결선(SPD 4.4V 직결)과 접지 바운스로 보드를 2장 태웠다.
 *  2) MCP4822를 아이솔레이터 건너편(컨트롤러 접지 도메인)에 두면, 아날로그
 *     신호가 절연장벽을 넘지 않고 디지털 SPI만 넘는다. MCU 핀이 TS 전압에
 *     물리적으로 노출되지 않으므로 같은 방식으로 죽을 수가 없다.
 *  3) PA4/PA5를 안 쓰므로 내부 DAC이 손상된 보드도 전부 재사용 가능하다.
 *
 * MCP4822 사양:
 *  - 2채널 12비트, 내장 2.048V 레퍼런스, VDD 2.7~5.5V
 *  - 게인 2배 설정 시 풀스케일 4.096V → ★DAC 코드가 곧 밀리볼트(1LSB=1mV)
 *  - 전원 투입 직후 셧다운(출력 풀다운) = 안전한 기본 상태
 *
 * ★MCP4822를 5V로 구동할 때 VIH = 0.7×VDD = 3.5V라 STM32의 3.3V로는 부족하다.
 *   아이솔레이터 2차측(VCC2)을 5V로 주면 출력이 5V 로직으로 나와 자동 해결된다.
 *   아이솔레이터 없이 직결 테스트할 때는 MCP4822를 3.3V로 구동할 것. */
#define DAC_SPI_SCK_PORT   GPIOC
#define DAC_SPI_SCK_PIN    GPIO_PIN_10   /* PC10 SPI3_SCK  (AF6) — CN7-1 */
#define DAC_SPI_MOSI_PORT  GPIOC
#define DAC_SPI_MOSI_PIN   GPIO_PIN_12   /* PC12 SPI3_MOSI (AF6) — CN7-3 */
#define DAC_CS_PORT        GPIOC
#define DAC_CS_PIN         GPIO_PIN_11   /* PC11 GPIO 출력, idle=High — CN7-2 */

/* MCP4822 16비트 명령어 비트 구성
 *   bit15 : A/B    0=DACA(VOA, 좌), 1=DACB(VOB, 우)
 *   bit14 : —      (무시)
 *   bit13 : GA     0=게인2배(4.096V 풀스케일), 1=게인1배(2.048V)
 *   bit12 : SHDN   1=활성, 0=셧다운
 *   bit11~0: 12비트 데이터 */
#define MCP4822_CH_A       0x0000u   /* VOA — 좌측 */
#define MCP4822_CH_B       0x8000u   /* VOB — 우측 */
#define MCP4822_GAIN2X     0x0000u   /* bit13=0 → 2배 (1LSB = 1mV) */
#define MCP4822_ACTIVE     0x1000u   /* bit12=1 */
#define MCP4822_DATA_MASK  0x0FFFu

#define DAC_SPI_TIMEOUT_MS 2u        /* 2바이트 전송은 ~12us, 여유 있게 */

/* ── IMU : WitMotion (USART3 + DMA RX Circular) ────────────────────── */
#define IMU_UART_TX_PORT   GPIOA
#define IMU_UART_TX_PIN    GPIO_PIN_0    /* PA0  UART4_TX (AF8) — CN7-28 */
#define IMU_UART_RX_PORT   GPIOA
#define IMU_UART_RX_PIN    GPIO_PIN_1    /* PA1  UART4_RX (AF8) — CN7-30 */
#define IMU_UART_BAUD      115200u

/* ── CAN1 ──────────────────────────────────────────────────────────── */
#define CAN_RX_PORT        GPIOA
#define CAN_RX_PIN         GPIO_PIN_11   /* PA11 CAN1_RX (AF9) */
#define CAN_TX_PORT        GPIOA
#define CAN_TX_PIN         GPIO_PIN_12   /* PA12 CAN1_TX (AF9) */

/* ── SD 카드 : SPI2 + FATFS ────────────────────────────────────────── */
#define SD_SPI_SCK_PORT    GPIOB
#define SD_SPI_SCK_PIN     GPIO_PIN_13   /* PB13 SPI2_SCK  (AF5) */
#define SD_SPI_MISO_PORT   GPIOB
#define SD_SPI_MISO_PIN    GPIO_PIN_14   /* PB14 SPI2_MISO (AF5) */
#define SD_SPI_MOSI_PORT   GPIOB
#define SD_SPI_MOSI_PIN    GPIO_PIN_15   /* PB15 SPI2_MOSI (AF5) */
#define SD_CS_PORT         GPIOB
#define SD_CS_PIN          GPIO_PIN_1    /* PB1 GPIO 출력, idle=High — CN10-24
                                          * CubeMX에서 User Label을 SD_CS로 두면
                                          * FATFS user_diskio가 SD_CS_GPIO_Port /
                                          * SD_CS_Pin 이름으로 참조한다. */

/* ── TV 토글 스위치 (내부 풀업, 눌림/ON = RESET) ───────────────────── */
/* ★2026-09: TV 토글 스위치를 Board A로 이전했다. 스위치가 조종석에 있어
 * 앞 보드 배선이 훨씬 짧고, 앞 보드는 이미 100Hz로 0x100 프레임을 보내고
 * 있어서 바이트 하나만 실으면 된다(SENSOR_FLAG_TV_SW).
 * → Board B의 PC13(CN7-23)은 이제 **미사용**이다. 아래 정의는 되돌릴 때를
 *   대비해 남겨두지만 코드에서 참조하지 않는다. Board B는 CAN_IsTVSwitchOn()
 *   으로 스위치 상태를 읽는다. */
#define TV_SWITCH_PORT     GPIOC
#define TV_SWITCH_PIN      GPIO_PIN_13   /* PC13 — 미사용(Board A로 이전) */
#define TV_SWITCH_ON_STATE GPIO_PIN_RESET

/* ── 상태 LED (외부, PA5는 DAC가 점유) ─────────────────────────────── */
#define LED_PORT           GPIOB
#define LED_PIN            GPIO_PIN_0    /* PB0 */


/* ── 좌/우 RPM 펄스 (컨트롤러 SPD 핀, TIM3 Input Capture, 16비트) ────
 * ★PB4/PB5는 ADC가 연결되지 않은 핀이라 **FT(5V 톨러런트)** 다. 절대최대정격이
 *  5.5V라 SPD 피크 4.4V가 여유 있게 들어온다 — 저항을 깜빡해도 핀이 안 죽는다.
 *  (PA0/PA1은 ADC 직결 TTa라 상한이 3.6V였고, 직렬저항 없이는 위험했다.)
 *
 * ★그래도 RC 저역통과는 넣을 것: 직렬 1kΩ + 핀-GND 간 10nF
 *   - 5V 톨러런스와 무관하게, SPD는 이 시스템에서 가장 노이즈가 심한 신호다
 *   - CN10에서 SD SPI(26/28/30)와 맞물려 있어 크로스토크 대책이 필요하다
 *   - 디지털 입력이라 직렬저항에 의한 정밀도 손실이 없다(DAC 라인과 다른 점)
 *
 * ★2026-08 실사고: SPD 배선을 RPM 핀이 아니라 DAC 핀에 한 칸 밀려 꽂은 채로
 *   구동해서 DAC 출력버퍼가 파손됐다(보드 교체함). 커넥터 키잉/라벨링으로
 *   물리적으로 못 바꿔 꽂게 만들 것. */
#define RPM_LEFT_PIN       GPIO_PIN_4    /* PB4  TIM3_CH1 (AF2) — CN10-27 */
#define RPM_RIGHT_PORT     GPIOB
#define RPM_RIGHT_PIN      GPIO_PIN_5    /* PB5  TIM3_CH2 (AF2) — CN10-29 */


#endif /* BOARD_CONFIG_H */
