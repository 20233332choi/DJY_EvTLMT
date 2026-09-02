#pragma once

#include <stdint.h>

#define DJY_CAN_ID_SENSOR_DATA      0x100u
#define DJY_CAN_ID_DRIVER_CONTROL   0x110u
#define DJY_CAN_ID_PIT_CONFIG       0x120u
#define DJY_CAN_ID_ESP_STATUS       0x121u
#define DJY_CAN_ID_REAR_STATUS      0x310u
#define DJY_CAN_ID_REAR_DRIVETRAIN  0x311u
#define DJY_CAN_ID_PIT_CONFIG_ACK   0x320u
#define DJY_PROTOCOL_VERSION 1u
#define DJY_CAN_DLC_ESP_STATUS 8u

enum {
    DJY_CONTROL_FLAG_TV_ENABLE = 1u << 0,
    DJY_CONTROL_FLAG_REGEN_ENABLE = 1u << 1,
};

enum {
    DJY_REAR_STATUS_CONTROL_FRESH = 1u << 0,
    DJY_REAR_STATUS_TV_ACTIVE = 1u << 1,
    DJY_REAR_STATUS_ED_ACTIVE = 1u << 2,
    DJY_REAR_STATUS_REGEN_READY = 1u << 3,
    DJY_REAR_STATUS_FAULT = 1u << 4,
};

enum {
    DJY_PIT_FLAG_TV_PERMITTED = 1u << 0,
    DJY_PIT_FLAG_REGEN_PERMITTED = 1u << 1,
};

enum {
    DJY_ESP_STATUS_PIT_ENABLE = 1u << 0,
    DJY_ESP_STATUS_WIFI_CONNECTED = 1u << 1,
    DJY_ESP_STATUS_CAN_RUNNING = 1u << 2,
    DJY_ESP_STATUS_FLAG_MASK = DJY_ESP_STATUS_PIT_ENABLE |
                               DJY_ESP_STATUS_WIFI_CONNECTED |
                               DJY_ESP_STATUS_CAN_RUNNING,
};

struct DjyEspStatus {
    uint8_t flags;
    uint8_t sequence;
    uint16_t uptime_100ms;
};

static inline uint16_t djy_u16(const uint8_t* data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static inline void djy_pack_u16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value & 0xffu);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

static inline uint8_t djy_crc8(const uint8_t* data, uint8_t length) {
    uint8_t crc = 0xffu;
    for (uint8_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x1du) : (uint8_t)(crc << 1);
        }
    }
    return (uint8_t)(crc ^ 0xffu);
}

static inline void djy_pack_esp_status(uint8_t data[8], const DjyEspStatus& status) {
    data[0] = status.flags & DJY_ESP_STATUS_FLAG_MASK;
    data[1] = status.sequence;
    data[2] = DJY_PROTOCOL_VERSION;
    data[3] = 0u;
    djy_pack_u16(&data[4], status.uptime_100ms);
    data[6] = 0u;
    data[7] = djy_crc8(data, 7u);
}
