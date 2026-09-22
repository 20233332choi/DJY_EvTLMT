#ifndef DJY_UART_PROTOCOL_H
#define DJY_UART_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* Dedicated ESP32 <-> Rear STM32 UART link.
 *
 * The pit computer refreshes this command through the ESP32 at 10 Hz.  Rear
 * accepts it only while valid frames keep arriving; loss of the link therefore
 * ramps the differential strength back to zero through ControlSettings.
 */
#define DJY_UART_MAGIC_0          0xd5u
#define DJY_UART_MAGIC_1          0x4au
#define DJY_UART_PROTOCOL_VERSION 1u
#define DJY_UART_TYPE_LIVE_TV     1u
#define DJY_UART_LIVE_TV_SIZE     9u

enum {
    DJY_UART_TV_FLAG_ENABLE = 1u << 0,
    DJY_UART_TV_FLAG_MASK   = DJY_UART_TV_FLAG_ENABLE
};

typedef struct {
    uint8_t sequence;
    uint8_t strength_percent;
    uint8_t limit_percent;
    uint8_t flags;
} DjyUartLiveTv;

/* CRC-8/SAE-J1850: poly=0x1D, init=0xFF, xorout=0xFF. */
static inline uint8_t djy_uart_crc8(const uint8_t *data, uint8_t length) {
    uint8_t crc = 0xffu;
    for (uint8_t i = 0u; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x1du)
                                : (uint8_t)(crc << 1);
        }
    }
    return (uint8_t)(crc ^ 0xffu);
}

static inline void djy_uart_pack_live_tv(uint8_t data[DJY_UART_LIVE_TV_SIZE],
                                         const DjyUartLiveTv *command) {
    data[0] = DJY_UART_MAGIC_0;
    data[1] = DJY_UART_MAGIC_1;
    data[2] = DJY_UART_PROTOCOL_VERSION;
    data[3] = DJY_UART_TYPE_LIVE_TV;
    data[4] = command->sequence;
    data[5] = command->strength_percent;
    data[6] = command->limit_percent;
    data[7] = command->flags & DJY_UART_TV_FLAG_MASK;
    data[8] = djy_uart_crc8(data, 8u);
}

static inline bool djy_uart_unpack_live_tv(DjyUartLiveTv *command,
                                           const uint8_t data[DJY_UART_LIVE_TV_SIZE]) {
    if (command == 0 || data == 0 ||
        data[0] != DJY_UART_MAGIC_0 || data[1] != DJY_UART_MAGIC_1 ||
        data[2] != DJY_UART_PROTOCOL_VERSION || data[3] != DJY_UART_TYPE_LIVE_TV ||
        data[8] != djy_uart_crc8(data, 8u) ||
        data[5] > 100u || data[6] > 100u ||
        (data[5] % 5u) != 0u || (data[6] % 5u) != 0u ||
        (data[7] & (uint8_t)~DJY_UART_TV_FLAG_MASK) != 0u) {
        return false;
    }
    command->sequence = data[4];
    command->strength_percent = data[5];
    command->limit_percent = data[6];
    command->flags = data[7];
    return true;
}

#endif /* DJY_UART_PROTOCOL_H */
