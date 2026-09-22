#ifndef DJY_CAN_PROTOCOL_H
#define DJY_CAN_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* Common 11-bit CAN identifiers. Keep the legacy sensor IDs stable because
 * the ESP32 telemetry gateway already consumes them. */
#define DJY_CAN_ID_SENSOR_DATA     0x100u
#define DJY_CAN_ID_DRIVER_CONTROL  0x110u
#define DJY_CAN_ID_PIT_CONFIG      0x120u
#define DJY_CAN_ID_ESP_STATUS      0x121u
#define DJY_CAN_ID_LEFT_RPM        0x200u
#define DJY_CAN_ID_RIGHT_RPM       0x201u
#define DJY_CAN_ID_HEARTBEAT       0x300u
#define DJY_CAN_ID_REAR_STATUS     0x310u
#define DJY_CAN_ID_REAR_DRIVETRAIN 0x311u
#define DJY_CAN_ID_PIT_CONFIG_ACK  0x320u

#define DJY_CAN_DLC_SENSOR_DATA     4u
#define DJY_CAN_DLC_DRIVER_CONTROL  8u
#define DJY_CAN_DLC_HEARTBEAT       1u
#define DJY_CAN_DLC_REAR_STATUS     8u
#define DJY_CAN_DLC_PIT_CONFIG      8u
#define DJY_CAN_DLC_ESP_STATUS      8u
#define DJY_CAN_DLC_REAR_DRIVETRAIN 8u

#define DJY_PROTOCOL_VERSION 1u

#define DJY_HB_STATUS_OK       0x00u
#define DJY_HB_STATUS_SAS_ERR  0x01u
#define DJY_HB_STATUS_TPS_ERR  0x02u

typedef enum {
    DJY_MODE_QUALIFYING = 0,
    DJY_MODE_RACE       = 1,
    DJY_MODE_CHARGE     = 2,
    DJY_MODE_ATTACK     = 3
} DjyDriveMode;

enum {
    DJY_CONTROL_FLAG_TV_ENABLE    = 1u << 0,
    DJY_CONTROL_FLAG_REGEN_ENABLE = 1u << 1,
    DJY_CONTROL_FLAG_MASK         = DJY_CONTROL_FLAG_TV_ENABLE |
                                    DJY_CONTROL_FLAG_REGEN_ENABLE
};

enum {
    DJY_PIT_FLAG_TV_PERMITTED    = 1u << 0,
    DJY_PIT_FLAG_REGEN_PERMITTED = 1u << 1,
    DJY_PIT_FLAG_MASK            = DJY_PIT_FLAG_TV_PERMITTED |
                                   DJY_PIT_FLAG_REGEN_PERMITTED
};

enum {
    DJY_ESP_STATUS_PIT_ENABLE     = 1u << 0,
    DJY_ESP_STATUS_WIFI_CONNECTED = 1u << 1,
    DJY_ESP_STATUS_CAN_RUNNING    = 1u << 2,
    DJY_ESP_STATUS_FLAG_MASK      = DJY_ESP_STATUS_PIT_ENABLE |
                                    DJY_ESP_STATUS_WIFI_CONNECTED |
                                    DJY_ESP_STATUS_CAN_RUNNING
};

enum {
    DJY_REAR_STATUS_CONTROL_FRESH = 1u << 0,
    DJY_REAR_STATUS_TV_ACTIVE     = 1u << 1,
    DJY_REAR_STATUS_ED_ACTIVE     = 1u << 2,
    DJY_REAR_STATUS_REGEN_READY   = 1u << 3,
    DJY_REAR_STATUS_FAULT         = 1u << 4
};

typedef struct {
    uint8_t tv_percent;       /* Requested final differential strength, 0..100 */
    uint8_t regen_percent;    /* Maximum requested regen strength, 0..100 */
    DjyDriveMode mode;
    uint8_t flags;
    uint8_t sequence;
} DjyDriverControl;

typedef struct {
    uint8_t tv_applied_percent;
    uint8_t regen_applied_percent;
    DjyDriveMode mode;
    uint8_t status_flags;
    uint8_t fault_code;
    uint8_t sequence;
} DjyRearStatus;

typedef struct {
    uint8_t tv_limit_percent;
    uint8_t regen_limit_percent;
    uint8_t tv_ramp_10pct_s;     /* 1..25 => 10..250 percent/s */
    uint8_t regen_ramp_5pct_s;   /* 1..20 => 5..100 percent/s */
    uint8_t flags;
    uint8_t sequence;
} DjyPitConfig;

typedef struct {
    uint8_t flags;
    uint8_t sequence;
    uint16_t uptime_100ms;
} DjyEspStatus;

static inline void djy_can_pack_u16(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value & 0xffu);
    buffer[1] = (uint8_t)((value >> 8) & 0xffu);
}

static inline uint16_t djy_can_unpack_u16(const uint8_t *buffer) {
    return (uint16_t)(buffer[0] | ((uint16_t)buffer[1] << 8));
}

/* CRC-8/SAE-J1850: poly=0x1D, init=0xFF, xorout=0xFF. */
static inline uint8_t djy_crc8(const uint8_t *data, uint8_t length) {
    uint8_t crc = 0xffu;
    for (uint8_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x1du)
                                : (uint8_t)(crc << 1);
        }
    }
    return (uint8_t)(crc ^ 0xffu);
}

static inline void djy_pack_driver_control(uint8_t data[8],
                                           const DjyDriverControl *control) {
    data[0] = control->tv_percent;
    data[1] = control->regen_percent;
    data[2] = (uint8_t)control->mode;
    data[3] = control->flags & DJY_CONTROL_FLAG_MASK;
    data[4] = control->sequence;
    data[5] = DJY_PROTOCOL_VERSION;
    data[6] = 0u;
    data[7] = djy_crc8(data, 7u);
}

static inline bool djy_unpack_driver_control(DjyDriverControl *control,
                                             const uint8_t data[8]) {
    if (data[7] != djy_crc8(data, 7u) ||
        data[5] != DJY_PROTOCOL_VERSION || data[6] != 0u ||
        data[0] > 100u || data[1] > 100u ||
        data[2] > (uint8_t)DJY_MODE_ATTACK ||
        (data[3] & (uint8_t)~DJY_CONTROL_FLAG_MASK) != 0u) {
        return false;
    }

    control->tv_percent    = data[0];
    control->regen_percent = data[1];
    control->mode          = (DjyDriveMode)data[2];
    control->flags         = data[3];
    control->sequence      = data[4];
    return true;
}

static inline void djy_pack_rear_status(uint8_t data[8],
                                        const DjyRearStatus *status) {
    data[0] = status->tv_applied_percent;
    data[1] = status->regen_applied_percent;
    data[2] = (uint8_t)status->mode;
    data[3] = status->status_flags;
    data[4] = status->fault_code;
    data[5] = status->sequence;
    data[6] = DJY_PROTOCOL_VERSION;
    data[7] = djy_crc8(data, 7u);
}

static inline bool djy_unpack_rear_status(DjyRearStatus *status,
                                          const uint8_t data[8]) {
    if (data[7] != djy_crc8(data, 7u) ||
        data[6] != DJY_PROTOCOL_VERSION || data[0] > 100u ||
        data[1] > 100u || data[2] > (uint8_t)DJY_MODE_ATTACK) {
        return false;
    }

    status->tv_applied_percent    = data[0];
    status->regen_applied_percent = data[1];
    status->mode                  = (DjyDriveMode)data[2];
    status->status_flags          = data[3];
    status->fault_code            = data[4];
    status->sequence              = data[5];
    return true;
}

static inline void djy_pack_pit_config(uint8_t data[8],
                                       const DjyPitConfig *config) {
    data[0] = config->tv_limit_percent;
    data[1] = config->regen_limit_percent;
    data[2] = config->tv_ramp_10pct_s;
    data[3] = config->regen_ramp_5pct_s;
    data[4] = config->flags & DJY_PIT_FLAG_MASK;
    data[5] = config->sequence;
    data[6] = DJY_PROTOCOL_VERSION;
    data[7] = djy_crc8(data, 7u);
}

static inline bool djy_unpack_pit_config(DjyPitConfig *config,
                                         const uint8_t data[8]) {
    if (data[7] != djy_crc8(data, 7u) || data[6] != DJY_PROTOCOL_VERSION ||
        data[0] > 100u || data[1] > 100u ||
        data[2] < 1u || data[2] > 25u || data[3] < 1u || data[3] > 20u ||
        (data[4] & (uint8_t)~DJY_PIT_FLAG_MASK) != 0u) {
        return false;
    }
    config->tv_limit_percent = data[0];
    config->regen_limit_percent = data[1];
    config->tv_ramp_10pct_s = data[2];
    config->regen_ramp_5pct_s = data[3];
    config->flags = data[4];
    config->sequence = data[5];
    return true;
}

/* ESP32 -> STM32 link heartbeat. It reports connectivity only and must never
 * be interpreted as a torque, regen, or DAC command. */
static inline void djy_pack_esp_status(uint8_t data[8],
                                       const DjyEspStatus *status) {
    data[0] = status->flags & DJY_ESP_STATUS_FLAG_MASK;
    data[1] = status->sequence;
    data[2] = DJY_PROTOCOL_VERSION;
    data[3] = 0u;
    djy_can_pack_u16(&data[4], status->uptime_100ms);
    data[6] = 0u;
    data[7] = djy_crc8(data, 7u);
}

static inline bool djy_unpack_esp_status(DjyEspStatus *status,
                                         const uint8_t data[8]) {
    if (data[7] != djy_crc8(data, 7u) ||
        data[2] != DJY_PROTOCOL_VERSION || data[3] != 0u || data[6] != 0u ||
        (data[0] & (uint8_t)~DJY_ESP_STATUS_FLAG_MASK) != 0u) {
        return false;
    }
    status->flags = data[0];
    status->sequence = data[1];
    status->uptime_100ms = djy_can_unpack_u16(&data[4]);
    return true;
}

#endif /* DJY_CAN_PROTOCOL_H */
