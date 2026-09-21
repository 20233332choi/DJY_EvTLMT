#include "esp_link.h"
#include <string.h>

static UART_HandleTypeDef *s_uart;
static uint8_t s_rx_byte;
static uint8_t s_frame[DJY_UART_LIVE_TV_SIZE];
static uint8_t s_length;
static volatile DjyUartLiveTv s_command;
static volatile uint32_t s_last_rx_ms;
/* ★static이면 Live Expression에서 못 읽는다(함수 호출도 불가). 전역으로 꺼낸다 —
 * CAN/IMU 진단 카운터와 같은 이유. 아래 접근 함수는 그대로 두어 팀원 코드와
 * 호환을 유지한다. */
volatile uint32_t g_esp_rx_count;
volatile uint32_t g_esp_crc_err;
/* ★원시 바이트 수 — "배선이 죽었나" vs "보드레이트가 틀렸나"를 가른다.
 * 매직(0xD5)이 아닌 바이트는 파서가 조용히 버리므로, 이게 없으면 둘을
 * 구분할 방법이 없다. byte가 늘고 rx가 0이면 형식/보드레이트 문제다. */
volatile uint32_t g_esp_byte_count;
/* ★수신 재무장 실패 / UART 에러 횟수 — 수신이 얼어붙는 경로를 추적한다. */
volatile uint32_t g_esp_rearm_fail;
volatile uint32_t g_esp_uart_err;

/* ★수신 재무장. 원본은 반환값을 버렸는데, HAL이 BUSY를 돌려주면 그 순간부터
 * 수신이 영원히 죽는다(바이트 카운터가 한 값에 얼어붙는다). IMU DMA에서
 * 똑같은 함정을 겪었으므로 여기서도 실패를 세고 강제로 되살린다. */
static void restart_receive(void) {
    if (s_uart == 0) return;
    if (HAL_UART_Receive_IT(s_uart, &s_rx_byte, 1u) == HAL_OK) return;

    ++g_esp_rearm_fail;
    /* RX recovery must not truncate the independent telemetry TX. */
    HAL_UART_AbortReceive(s_uart);
    s_uart->RxState   = HAL_UART_STATE_READY;
    s_uart->ErrorCode = HAL_UART_ERROR_NONE;
    (void)HAL_UART_Receive_IT(s_uart, &s_rx_byte, 1u);
}

/* ★메인 루프에서 주기적으로 호출. 어떤 이유로든 수신이 무장 해제된 채로
 * 남아 있으면(에러 콜백이 안 왔거나 재무장이 실패했거나) 여기서 되살린다.
 * ESP32가 아예 안 붙어 있어도 무해하다 — 그냥 다시 무장만 한다. */
void EspLink_Watchdog(void) {
    if (s_uart == 0) return;
    if (s_uart->RxState != HAL_UART_STATE_BUSY_RX) {
        s_length = 0u;
        restart_receive();
    }
}

static void consume_byte(uint8_t value) {
    ++g_esp_byte_count;
    if (s_length == 0u) {
        if (value == DJY_UART_MAGIC_0) s_frame[s_length++] = value;
        return;
    }
    if (s_length == 1u && value != DJY_UART_MAGIC_1) {
        s_length = (value == DJY_UART_MAGIC_0) ? 1u : 0u;
        return;
    }

    s_frame[s_length++] = value;
    if (s_length < DJY_UART_LIVE_TV_SIZE) return;

    DjyUartLiveTv decoded;
    if (djy_uart_unpack_live_tv(&decoded, s_frame)) {
        s_command = decoded;
        s_last_rx_ms = HAL_GetTick();
        ++g_esp_rx_count;
    } else {
        ++g_esp_crc_err;
    }
    s_length = 0u;
}

void EspLink_Init(UART_HandleTypeDef *uart) {
    s_uart = uart;
    s_length = 0u;
    s_last_rx_ms = 0u;
    g_esp_rx_count = 0u;
    g_esp_crc_err = 0u;
    g_esp_byte_count = 0u;
    g_esp_rearm_fail = 0u;
    g_esp_uart_err = 0u;
    memset((void *)&s_command, 0, sizeof(s_command));
    restart_receive();
}

bool EspLink_GetLiveTv(DjyUartLiveTv *command) {
    if (command == 0) return false;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    *command = s_command;
    uint32_t last_rx_ms = s_last_rx_ms;
    __set_PRIMASK(mask);
    return last_rx_ms != 0u && (HAL_GetTick() - last_rx_ms) <= ESP_LINK_TIMEOUT_MS;
}

bool EspLink_IsFresh(void) {
    uint32_t last_rx_ms = s_last_rx_ms;
    return last_rx_ms != 0u && (HAL_GetTick() - last_rx_ms) <= ESP_LINK_TIMEOUT_MS;
}

uint32_t EspLink_GetRxCount(void) { return g_esp_rx_count; }
uint32_t EspLink_GetCrcErrorCount(void) { return g_esp_crc_err; }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart == s_uart) {
        consume_byte(s_rx_byte);
        restart_receive();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart == s_uart) {
        /* ★원본은 ORE만 지웠다. 선이 떠 있거나 연결 순간에 걸리는 프레이밍(FE)·
         * 노이즈(NE) 에러는 안 지워져서, 그 상태로 재무장하면 곧바로 다시
         * 에러로 떨어질 수 있다. 세 개를 다 지운다. */
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        ++g_esp_uart_err;
        s_length = 0u;          /* 깨진 프레임 잔재를 버리고 처음부터 */
        restart_receive();
    }
}
