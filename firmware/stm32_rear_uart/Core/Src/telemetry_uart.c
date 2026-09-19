#include "telemetry_uart.h"
#include <string.h>

static UART_HandleTypeDef telemetry_uart;
static uint8_t tx_buffer[512];
static uint8_t rx_byte;
static uint8_t rx_frame[DJY_UART_LIVE_TV_SIZE];
static uint8_t rx_length;
static volatile DjyUartLiveTv live_tv_command;
static volatile uint32_t last_command_ms;
static volatile uint32_t rx_count;
static volatile uint32_t crc_error_count;
static uint8_t ready;

static void restart_receive(void)
{
    if (ready) (void)HAL_UART_Receive_IT(&telemetry_uart, &rx_byte, 1u);
}

static void consume_byte(uint8_t value)
{
    if (rx_length == 0u) {
        if (value == DJY_UART_MAGIC_0) rx_frame[rx_length++] = value;
        return;
    }
    if (rx_length == 1u && value != DJY_UART_MAGIC_1) {
        rx_length = (value == DJY_UART_MAGIC_0) ? 1u : 0u;
        return;
    }

    rx_frame[rx_length++] = value;
    if (rx_length < DJY_UART_LIVE_TV_SIZE) return;

    DjyUartLiveTv decoded;
    if (djy_uart_unpack_live_tv(&decoded, rx_frame)) {
        live_tv_command = decoded;
        last_command_ms = HAL_GetTick();
        ++rx_count;
    } else {
        ++crc_error_count;
    }
    rx_length = 0u;
}

HAL_StatusTypeDef TelemetryUart_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);
    telemetry_uart.Instance = USART1;
    telemetry_uart.Init.BaudRate = 115200;
    telemetry_uart.Init.WordLength = UART_WORDLENGTH_8B;
    telemetry_uart.Init.StopBits = UART_STOPBITS_1;
    telemetry_uart.Init.Parity = UART_PARITY_NONE;
    telemetry_uart.Init.Mode = UART_MODE_TX_RX;
    telemetry_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    telemetry_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_StatusTypeDef status = HAL_UART_Init(&telemetry_uart);
    if (status == HAL_OK) {
        // Lower priority than vehicle control/timer interrupts.
        HAL_NVIC_SetPriority(USART1_IRQn, 15, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
        ready = 1;
        restart_receive();
    }
    return status;
}

bool TelemetryUart_GetLiveTv(DjyUartLiveTv *command)
{
    if (!command) return false;
    __disable_irq();
    *command = live_tv_command;
    uint32_t received_ms = last_command_ms;
    __enable_irq();
    return received_ms != 0u &&
           (HAL_GetTick() - received_ms) <= TELEMETRY_UART_COMMAND_TIMEOUT_MS;
}

bool TelemetryUart_IsCommandFresh(void)
{
    uint32_t received_ms = last_command_ms;
    return received_ms != 0u &&
           (HAL_GetTick() - received_ms) <= TELEMETRY_UART_COMMAND_TIMEOUT_MS;
}

uint32_t TelemetryUart_GetRxCount(void) { return rx_count; }
uint32_t TelemetryUart_GetCrcErrorCount(void) { return crc_error_count; }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &telemetry_uart) {
        consume_byte(rx_byte);
        restart_receive();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &telemetry_uart) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        restart_receive();
    }
}

HAL_StatusTypeDef TelemetryUart_Send(const uint8_t *data, uint16_t length)
{
    if (!ready || !data || !length || length > sizeof(tx_buffer)) return HAL_ERROR;
    // Drop a sample if busy; never wait for ESP or block vehicle control.
    if (telemetry_uart.gState != HAL_UART_STATE_READY) return HAL_BUSY;
    memcpy(tx_buffer, data, length);
    return HAL_UART_Transmit_IT(&telemetry_uart, tx_buffer, length);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&telemetry_uart);
}
