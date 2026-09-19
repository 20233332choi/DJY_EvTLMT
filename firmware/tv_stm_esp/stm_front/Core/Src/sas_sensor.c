#include "sas_sensor.h"
#include "board_config.h"
#include "main.h"            /* HAL 핸들 */

/* CubeMX: SPI1 Mode 1, 16-bit, ~1MHz, MSB first, NSS=Software
 * 핀은 board_config.h 참조 — PB3/PB4/PB5로 리맵(PA5는 LD2와 충돌).
 * CS는 SAS_CS_PORT/SAS_CS_PIN(PB6)을 GPIO로 직접 제어한다. */
extern SPI_HandleTypeDef hspi1;

#define AS5147_REG_NOP      0x0000u
#define AS5147_REG_ERRFL    0x0001u
#define AS5147_REG_ANGLECOM 0x3FFFu
#define AS5147_FLAG_READ    0x4000u  /* command bit14: 1=read */
#define AS5147_FLAG_ERROR   0x4000u  /* response bit14: error */
#define AS5147_PARITY_BIT   0x8000u
#define AS5147_DATA_MASK    0x3FFFu

static uint16_t s_last_valid = 0;
static bool     s_error      = false;

/* 16비트 even parity */
static inline uint8_t even_parity(uint16_t v) {
    v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (uint8_t)(v & 1u);
}

/* read 명령 프레임 생성 (bit14=read, bit15=even parity) */
static inline uint16_t cmd_read(uint16_t addr) {
    uint16_t c = addr | AS5147_FLAG_READ;
    if (even_parity(c)) c |= AS5147_PARITY_BIT;
    return c;
}

/* 16비트 1프레임 송수신. 응답은 '직전' 명령에 대한 데이터(파이프라인). */
static uint16_t sas_xfer(uint16_t cmd) {
    uint16_t rx = 0;
    HAL_GPIO_WritePin(SAS_CS_PORT, SAS_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)&cmd, (uint8_t *)&rx, 1, 2);
    HAL_GPIO_WritePin(SAS_CS_PORT, SAS_CS_PIN, GPIO_PIN_SET);
    return rx;
}

void SAS_Init(void) {
    HAL_GPIO_WritePin(SAS_CS_PORT, SAS_CS_PIN, GPIO_PIN_SET);
    (void)SAS_ReadAngle();   /* 파이프라인 정렬용 더미 2회 */
    (void)SAS_ReadAngle();
}

uint16_t SAS_ReadAngle(void) {
    const uint16_t cmd = cmd_read(AS5147_REG_ANGLECOM); /* = 0xFFFF */

    sas_xfer(cmd);                 /* 1) 명령 전송(직전 응답은 버림) */
    uint16_t resp = sas_xfer(cmd); /* 2) ANGLECOM 응답 수신 */

    /* 응답 패리티: bit0..14의 even parity == bit15 */
    uint8_t par = even_parity(resp & 0x7FFFu);
    if (par != ((resp & AS5147_PARITY_BIT) ? 1u : 0u)) {
        s_error = true;
        return s_last_valid;
    }
    /* 에러 플래그(bit14) → ERRFL 읽어 클리어 후 이전값 유지 */
    if (resp & AS5147_FLAG_ERROR) {
        s_error = true;
        sas_xfer(cmd_read(AS5147_REG_ERRFL));
        sas_xfer(cmd_read(AS5147_REG_NOP));
        return s_last_valid;
    }
    s_error = false;
    s_last_valid = resp & AS5147_DATA_MASK;
    return s_last_valid;
}

bool SAS_HasError(void) { return s_error; }
