#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { struct { uint32_t BaudRate; } Init; uint32_t ErrorCode; } UART_HandleTypeDef;
typedef struct { uint32_t remaining; } DMA_HandleTypeDef;
UART_HandleTypeDef huart3, huart2;
DMA_HandleTypeDef hdma_usart3_rx;
static uint32_t tick, sensor_baud = 9600, stream_idx;
static unsigned sensor_tx, dma_starts;
static bool corrupt, gyro_only, silent, uart_error, init_error;
static uint8_t stream[22];
static uint32_t HAL_GetTick(void) { return tick++; }
static void HAL_Delay(uint32_t ms) { tick += ms; }
static void Error_Handler(void) { CHECK(false); }
#define HAL_UART_ERROR_NONE 0u
#define __HAL_UART_CLEAR_OREFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_NEFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_FEFLAG(h) ((void)(h))
#define __HAL_DMA_GET_COUNTER(h) ((h)->remaining)
static HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *h) {
    stream_idx = 0;
    return init_error && h->Init.BaudRate != 115200u ? HAL_ERROR : HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *b,
                                        uint16_t size, uint32_t timeout) {
    CHECK(h == &huart3 && size == 1);
    ++tick;
    if (uart_error) return HAL_ERROR;
    if (silent || h->Init.BaudRate != sensor_baud) { tick += timeout; return HAL_TIMEOUT; }
    *b = stream[stream_idx++ % sizeof(stream)];
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *data,
                                         uint16_t size, uint32_t timeout) {
    (void)data; (void)size; (void)timeout;
    if (h == &huart3) ++sensor_tx;
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef *h) { (void)h; return HAL_OK; }
static HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef *h, uint8_t *b, uint16_t size) {
    (void)h; (void)b; hdma_usart3_rx.remaining = size; ++dma_starts; return HAL_OK;
}
#include "imu_sensor.c"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    sensor_baud = (uint32_t)strtoul(argv[1], NULL, 10);
    corrupt = !strcmp(argv[1], "corrupt");
    gyro_only = !strcmp(argv[1], "gyro_only");
    silent = !strcmp(argv[1], "silent");
    uart_error = !strcmp(argv[1], "uart_error");
    init_error = !strcmp(argv[1], "init_error");
    bool expected = sensor_baud != 0;
    if (!expected) sensor_baud = 9600;
    for (unsigned p = 0; p < 2; ++p) {
        uint8_t *b = stream + 11 * p;
        b[0] = 0x55; b[1] = (p == 0 && !gyro_only) ? 0x51 : 0x52;
        b[2] = 1; b[6] = 2;
        for (unsigned i = 0; i < 10; ++i) b[10] += b[i];
        if (corrupt) b[10] ^= 1;
    }
    /* Unsigned timeout arithmetic also has to work across tick wrap. */
    tick = UINT32_MAX - 100;
    CHECK(imu_detect_baud() == expected);
    CHECK(huart3.Init.BaudRate == (expected ? sensor_baud : 115200u));
    CHECK(sensor_tx == 0); /* probing cannot configure the sensor */
    CHECK(s_pkt_ok == 0 && s_gyro_ok == 0 && s_acc_ok == 0);
    tick = 1;
    CHECK(!IMU_IsValid());
    IMU_Init();
    CHECK(dma_starts == 1);
    CHECK(sensor_tx == (expected && sensor_baud == 115200u ? 2u : 0u));
    CHECK(!IMU_IsValid()); /* probe packets must never enter control state */
    uint8_t gyro[11] = {0x55, 0x52};
    gyro[10] = 0xa7;
    CHECK(parse_packet(gyro));
    CHECK(IMU_IsValid());
    tick += IMU_TIMEOUT_MS + 1;
    CHECK(!IMU_IsValid());
    puts("IMU passive probe and validity PASS");
    return 0;
}
