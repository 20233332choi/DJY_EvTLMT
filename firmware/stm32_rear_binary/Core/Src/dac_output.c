#include "dac_output.h"
#include "board_config.h"
#include "vehicle_params.h"
#include "common_types.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* 백엔드는 vehicle_params.h의 DAC_USE_INTERNAL로 선택한다.
 *  0 = 외부 MCP4822 (SPI3: SCK=PB3, MOSI=PB5, CS=PB4) — 정식 구성
 *  1 = STM32 내부 DAC (PA4/PA5) — 임시/진단 전용
 * 위쪽 API(DAC_SetLeftThrottle 등)는 두 경우 모두 동일하므로, 이 파일 밖의
 * 어떤 코드도 백엔드를 알 필요가 없다. */
#if DAC_USE_INTERNAL
extern DAC_HandleTypeDef hdac;
#else
extern SPI_HandleTypeDef hspi3;
#endif

/* 진단용 — Live Expression으로 확인. SPI가 실패하면 계속 올라간다. */
volatile uint32_t g_dac_spi_fail = 0;

static uint16_t volt_to_code(float v) {
    float c = v * DAC_CODE_PER_V;    /* 환산비는 백엔드에 따라 다름 */
    return (uint16_t)CLAMP(c, 0.0f, (float)DAC_RESOLUTION);
}

#if DAC_USE_INTERNAL
/* ── 내부 DAC 백엔드 (임시) ─────────────────────────────────────── */
static void dac_write_left(uint16_t code) {
    HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                     CLAMP(code, 0, DAC_RESOLUTION));
}
static void dac_write_right(uint16_t code) {
    HAL_DAC_SetValue(&hdac, DAC_CHANNEL_2, DAC_ALIGN_12B_R,
                     CLAMP(code, 0, DAC_RESOLUTION));
}
static void dac_backend_init(void) {
    HAL_DAC_Start(&hdac, DAC_CHANNEL_1);
    HAL_DAC_Start(&hdac, DAC_CHANNEL_2);
}

#else
/* ── 외부 MCP4822 백엔드 (정식) ─────────────────────────────────────
 * CS를 내렸다가 16비트를 보내고 다시 올리면, 그 상승 에지에서 출력이 갱신된다
 * (LDAC 핀은 TS측 GND에 직결 전제 — board_config.h 배선 주석 참고). */
static void mcp4822_write(uint16_t cmd) {
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFFu) };

    HAL_GPIO_WritePin(DAC_CS_PORT, DAC_CS_PIN, GPIO_PIN_RESET);
    if (HAL_SPI_Transmit(&hspi3, buf, 2, DAC_SPI_TIMEOUT_MS) != HAL_OK) {
        g_dac_spi_fail++;
    }
    HAL_GPIO_WritePin(DAC_CS_PORT, DAC_CS_PIN, GPIO_PIN_SET);
}
static void mcp4822_set(uint16_t channel_bit, uint16_t code) {
    if (code > DAC_RESOLUTION) code = DAC_RESOLUTION;
    mcp4822_write(channel_bit | MCP4822_GAIN2X | MCP4822_ACTIVE
                  | (code & MCP4822_DATA_MASK));
}
static void dac_write_left(uint16_t code)  { mcp4822_set(MCP4822_CH_A, code); }
static void dac_write_right(uint16_t code) { mcp4822_set(MCP4822_CH_B, code); }
static void dac_backend_init(void) {
    /* CS는 MX_GPIO_Init()에서 이미 High로 초기화되지만, 순서에 의존하지 않도록
     * 여기서 한 번 더 명시한다. CS가 Low인 채로 SPI가 돌면 쓰레기가 들어간다. */
    HAL_GPIO_WritePin(DAC_CS_PORT, DAC_CS_PIN, GPIO_PIN_SET);
    HAL_Delay(1);
}
#endif

void DAC_Output_Init(void) {
    dac_backend_init();
    DAC_SetSafeState();
}

void DAC_SetLeftThrottle(uint16_t dac_code)  { dac_write_left(dac_code); }
void DAC_SetRightThrottle(uint16_t dac_code) { dac_write_right(dac_code); }

void DAC_SetSafeState(void) {
    uint16_t off = volt_to_code(V_THROTTLE_OFF);   /* 0.90V → 모터 오프 */
    dac_write_left(off);
    dac_write_right(off);
}

/* =====================================================================
 *  ★TEMP 진단 도구: DAC 좌우 채널 동시 스윕 (DAC_SELFTEST_MODE 전용)
 * ---------------------------------------------------------------------
 *  Board A도, CAN도, SD카드도 필요 없다. 좌우 채널에 "완전히 동일한 코드"를
 *  써 넣고 4초씩 유지하면서 USART2로 무엇을 쓰고 있는지 출력한다.
 *
 *  판정 방법 — 각 단계에서 VOA와 VOB를 재보면:
 *    · 둘 다 expect 값과 일치      → 두 채널 모두 정상
 *    · 한쪽만 따라오고 다른 쪽 고정 → 그 채널 하드웨어 손상
 *    · 둘 다 안 따라옴             → SPI 배선/전원 문제 (g_dac_spi_fail 확인)
 *
 *  ★MCP4822는 코드가 곧 mV이므로 expect 값이 그대로 읽힌다.
 *  리턴하지 않는다. 확인 끝나면 DAC_SELFTEST_MODE를 0으로 되돌릴 것.
 * ===================================================================== */
void DAC_SelfTest(void) {
    extern UART_HandleTypeDef huart2;
    /* 목표 전압으로 지정한다 — 코드 환산은 백엔드에 따라 자동으로 달라진다.
     * 0V / 0.9V(off) / 1.5V / 2.0V / 3.0V(7kW 상당) */
    static const float volts[] = { 0.0f, 0.9f, 1.5f, 2.0f, 3.0f };
    char line[112];

#if DAC_USE_INTERNAL
    const char *hdr = "\r\n=== 내부 DAC selftest (PA4/PA5가 항상 같아야 정상) ===\r\n";
#else
    const char *hdr = "\r\n=== MCP4822 selftest (VOA/VOB가 항상 같아야 정상) ===\r\n";
#endif
    HAL_UART_Transmit(&huart2, (uint8_t *)hdr, (uint16_t)strlen(hdr), 100);

    while (1) {
        for (unsigned i = 0; i < sizeof(volts) / sizeof(volts[0]); i++) {
            uint16_t code = volt_to_code(volts[i]);

            /* 좌우에 완전히 동일한 값 — 여기서 좌우가 갈리면 100% 하드웨어다 */
            DAC_SetLeftThrottle(code);
            DAC_SetRightThrottle(code);

            int mv = (int)(volts[i] * 1000.0f + 0.5f);
            int n = snprintf(line, sizeof(line),
                             "expect %d.%03dV  (code=%4u)   spi_fail=%lu\r\n",
                             mv / 1000, mv % 1000, code,
                             (unsigned long)g_dac_spi_fail);
            if (n > 0) HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)n, 100);

            HAL_Delay(4000);   /* 측정할 시간 */
        }
    }
}
