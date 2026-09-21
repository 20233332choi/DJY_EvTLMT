#ifndef DJY_TELEMETRY_PROTOCOL_H
#define DJY_TELEMETRY_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Identical copy in stm_back/Core/Inc and ESP include; checked by host tests.
 * Rear -> ESP only. Existing 9-byte LIVE_TV commands are unchanged.
 * Wire v1: D5 4A, version=1, type=2, uint16 LE payload length=230,
 * fields below, 19 timing words, CRC16 LE. All multi-byte values are LE.
 * No native structs/floats are copied to the wire. Signed integers use two's
 * complement; *_milli preserves the old ASCII x1000 scale (steer is radians).
 * snapshot_us is Rear main-loop snapshot time, NOT a sensor acquisition time.
 */
#define DJY_TELEMETRY_BAUD 460800u
#define DJY_TELEMETRY_SIZE 238u
#define DJY_TELEMETRY_PAYLOAD_SIZE (DJY_TELEMETRY_SIZE - 8u)
#define DJY_TELEMETRY_TIMING_WORDS 19u
#define DJY_TM_IMU_VALID 1u
#define DJY_TM_SAS_VALID 2u
#define DJY_TM_RPM_LEFT_VALID 4u
#define DJY_TM_RPM_RIGHT_VALID 8u
#define DJY_TM_CONTROL_FRESH 16u
#define DJY_TM_TV_ACTIVE 32u
#define DJY_TM_ED_ACTIVE 64u
#define DJY_TM_FLAGS_MASK 127u
typedef struct {
    uint32_t sequence; /* wire 6..9 */
    uint64_t snapshot_us; /* wire 10..17 */
    uint32_t tx_skipped; /* wire 18..21 */
    uint16_t rpm_left; /* wire 22..23 */
    uint16_t rpm_right; /* wire 24..25 */
    uint16_t tps_raw; /* wire 26..27 */
    uint16_t tps_idle; /* wire 28..29 */
    uint16_t sas_raw; /* wire 30..31 */
    uint16_t sas_center; /* wire 32..33 */
    uint16_t dac_left; /* wire 34..35 */
    uint16_t dac_right; /* wire 36..37 */
    uint16_t traction_milli; /* wire 38..39 */
    uint8_t tps_pct; /* wire 40 */
    uint8_t flags; /* wire 41 */
    uint8_t fault; /* wire 42 */
    uint8_t command_sequence; /* wire 43 */
    uint8_t requested; /* wire 44 */
    uint8_t limit; /* wire 45 */
    uint8_t applied; /* wire 46 */
    uint8_t timing_valid; /* wire 47 */
    int32_t yaw_milli; /* wire 48..51 */
    int32_t lat_milli; /* wire 52..55 */
    int32_t ax_milli; /* wire 56..59 */
    int32_t ay_milli; /* wire 60..63 */
    int32_t az_milli; /* wire 64..67 */
    int32_t speed_milli; /* wire 68..71 */
    int32_t desired_yaw_milli; /* wire 72..75 */
    int32_t yaw_error_milli; /* wire 76..79 */
    int32_t delta_power_milli; /* wire 80..83 */
    int32_t power_left_milli; /* wire 84..87 */
    int32_t power_right_milli; /* wire 88..91 */
    int32_t steer_milli; /* wire 92..95 */
    int32_t kp_milli; /* wire 96..99 */
    int32_t ki_milli; /* wire 100..103 */
    int32_t kd_milli; /* wire 104..107 */
    uint32_t capture_left; /* wire 108..111 */
    uint32_t capture_right; /* wire 112..115 */
    uint32_t glitch_left; /* wire 116..119 */
    uint32_t glitch_right; /* wire 120..123 */
    uint32_t command_rx; /* wire 124..127 */
    uint32_t command_errors; /* wire 128..131 */
    uint32_t can_rx; /* wire 132..135 */
    uint32_t can_errors; /* wire 136..139 */
    uint32_t can_status; /* wire 140..143 */
    uint32_t imu_gyro_ok; /* wire 144..147 */
    uint32_t imu_pkt_bad; /* wire 148..151 */
    uint32_t imu_resync; /* wire 152..155 */
    uint32_t imu_dma_restart; /* wire 156..159 */
    uint32_t timing[DJY_TELEMETRY_TIMING_WORDS]; /* wire 160..235; word 11 is int32 drift */
} DjyTelemetry;

static inline uint8_t djy_tm_get_u8(const uint8_t *p) { return p[0]; }
static inline uint16_t djy_tm_get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t djy_tm_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline int32_t djy_tm_signed(uint32_t v) {
    return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v);
}
static inline int32_t djy_tm_get_i32(const uint8_t *p) { return djy_tm_signed(djy_tm_get_u32(p)); }
static inline uint64_t djy_tm_get_u64(const uint8_t *p) {
    return (uint64_t)djy_tm_get_u32(p) | (uint64_t)djy_tm_get_u32(p + 4) << 32;
}
static inline void djy_tm_put_u8(uint8_t *p, uint8_t v) { p[0] = v; }
static inline void djy_tm_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void djy_tm_put_u32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static inline void djy_tm_put_i32(uint8_t *p, int32_t v) { djy_tm_put_u32(p, (uint32_t)v); }
static inline void djy_tm_put_u64(uint8_t *p, uint64_t v) {
    djy_tm_put_u32(p, (uint32_t)v); djy_tm_put_u32(p + 4, (uint32_t)(v >> 32));
}
/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection/xorout.
 * Covers header + payload, excluding the final two CRC bytes. */
static inline uint16_t djy_tm_crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint32_t)crc << 1) ^ 0x1021u : (uint32_t)crc << 1);
    }
    return crc;
}
static inline bool djy_tm_header_valid(const uint8_t *data) {
    return data[0] == 0xd5u && data[1] == 0x4au && data[2] == 1u &&
           data[3] == 2u && djy_tm_get_u16(data + 4) == DJY_TELEMETRY_PAYLOAD_SIZE;
}
static inline bool djy_tm_frame_valid(const uint8_t *data, size_t size) {
    return data && size == DJY_TELEMETRY_SIZE && djy_tm_header_valid(data) &&
           djy_tm_get_u16(data + size - 2) == djy_tm_crc16(data, size - 2);
}
static inline void djy_telemetry_pack(uint8_t out[DJY_TELEMETRY_SIZE], const DjyTelemetry *value) {
    out[0] = 0xd5u; out[1] = 0x4au; out[2] = 1u; out[3] = 2u;
    djy_tm_put_u16(out + 4, DJY_TELEMETRY_PAYLOAD_SIZE);
    djy_tm_put_u32(out + 6, value->sequence);
    djy_tm_put_u64(out + 10, value->snapshot_us);
    djy_tm_put_u32(out + 18, value->tx_skipped);
    djy_tm_put_u16(out + 22, value->rpm_left);
    djy_tm_put_u16(out + 24, value->rpm_right);
    djy_tm_put_u16(out + 26, value->tps_raw);
    djy_tm_put_u16(out + 28, value->tps_idle);
    djy_tm_put_u16(out + 30, value->sas_raw);
    djy_tm_put_u16(out + 32, value->sas_center);
    djy_tm_put_u16(out + 34, value->dac_left);
    djy_tm_put_u16(out + 36, value->dac_right);
    djy_tm_put_u16(out + 38, value->traction_milli);
    djy_tm_put_u8(out + 40, value->tps_pct);
    djy_tm_put_u8(out + 41, value->flags);
    djy_tm_put_u8(out + 42, value->fault);
    djy_tm_put_u8(out + 43, value->command_sequence);
    djy_tm_put_u8(out + 44, value->requested);
    djy_tm_put_u8(out + 45, value->limit);
    djy_tm_put_u8(out + 46, value->applied);
    djy_tm_put_u8(out + 47, value->timing_valid);
    djy_tm_put_i32(out + 48, value->yaw_milli);
    djy_tm_put_i32(out + 52, value->lat_milli);
    djy_tm_put_i32(out + 56, value->ax_milli);
    djy_tm_put_i32(out + 60, value->ay_milli);
    djy_tm_put_i32(out + 64, value->az_milli);
    djy_tm_put_i32(out + 68, value->speed_milli);
    djy_tm_put_i32(out + 72, value->desired_yaw_milli);
    djy_tm_put_i32(out + 76, value->yaw_error_milli);
    djy_tm_put_i32(out + 80, value->delta_power_milli);
    djy_tm_put_i32(out + 84, value->power_left_milli);
    djy_tm_put_i32(out + 88, value->power_right_milli);
    djy_tm_put_i32(out + 92, value->steer_milli);
    djy_tm_put_i32(out + 96, value->kp_milli);
    djy_tm_put_i32(out + 100, value->ki_milli);
    djy_tm_put_i32(out + 104, value->kd_milli);
    djy_tm_put_u32(out + 108, value->capture_left);
    djy_tm_put_u32(out + 112, value->capture_right);
    djy_tm_put_u32(out + 116, value->glitch_left);
    djy_tm_put_u32(out + 120, value->glitch_right);
    djy_tm_put_u32(out + 124, value->command_rx);
    djy_tm_put_u32(out + 128, value->command_errors);
    djy_tm_put_u32(out + 132, value->can_rx);
    djy_tm_put_u32(out + 136, value->can_errors);
    djy_tm_put_u32(out + 140, value->can_status);
    djy_tm_put_u32(out + 144, value->imu_gyro_ok);
    djy_tm_put_u32(out + 148, value->imu_pkt_bad);
    djy_tm_put_u32(out + 152, value->imu_resync);
    djy_tm_put_u32(out + 156, value->imu_dma_restart);
    for (unsigned i = 0; i < DJY_TELEMETRY_TIMING_WORDS; ++i)
        djy_tm_put_u32(out + 160 + 4u * i, value->timing[i]);
    djy_tm_put_u16(out + DJY_TELEMETRY_SIZE - 2, djy_tm_crc16(out, DJY_TELEMETRY_SIZE - 2));
}
static inline bool djy_telemetry_unpack(DjyTelemetry *out, const uint8_t *data, size_t size) {
    if (!out || !djy_tm_frame_valid(data, size)) return false;
    DjyTelemetry value;
    value.sequence = djy_tm_get_u32(data + 6);
    value.snapshot_us = djy_tm_get_u64(data + 10);
    value.tx_skipped = djy_tm_get_u32(data + 18);
    value.rpm_left = djy_tm_get_u16(data + 22);
    value.rpm_right = djy_tm_get_u16(data + 24);
    value.tps_raw = djy_tm_get_u16(data + 26);
    value.tps_idle = djy_tm_get_u16(data + 28);
    value.sas_raw = djy_tm_get_u16(data + 30);
    value.sas_center = djy_tm_get_u16(data + 32);
    value.dac_left = djy_tm_get_u16(data + 34);
    value.dac_right = djy_tm_get_u16(data + 36);
    value.traction_milli = djy_tm_get_u16(data + 38);
    value.tps_pct = djy_tm_get_u8(data + 40);
    value.flags = djy_tm_get_u8(data + 41);
    value.fault = djy_tm_get_u8(data + 42);
    value.command_sequence = djy_tm_get_u8(data + 43);
    value.requested = djy_tm_get_u8(data + 44);
    value.limit = djy_tm_get_u8(data + 45);
    value.applied = djy_tm_get_u8(data + 46);
    value.timing_valid = djy_tm_get_u8(data + 47);
    value.yaw_milli = djy_tm_get_i32(data + 48);
    value.lat_milli = djy_tm_get_i32(data + 52);
    value.ax_milli = djy_tm_get_i32(data + 56);
    value.ay_milli = djy_tm_get_i32(data + 60);
    value.az_milli = djy_tm_get_i32(data + 64);
    value.speed_milli = djy_tm_get_i32(data + 68);
    value.desired_yaw_milli = djy_tm_get_i32(data + 72);
    value.yaw_error_milli = djy_tm_get_i32(data + 76);
    value.delta_power_milli = djy_tm_get_i32(data + 80);
    value.power_left_milli = djy_tm_get_i32(data + 84);
    value.power_right_milli = djy_tm_get_i32(data + 88);
    value.steer_milli = djy_tm_get_i32(data + 92);
    value.kp_milli = djy_tm_get_i32(data + 96);
    value.ki_milli = djy_tm_get_i32(data + 100);
    value.kd_milli = djy_tm_get_i32(data + 104);
    value.capture_left = djy_tm_get_u32(data + 108);
    value.capture_right = djy_tm_get_u32(data + 112);
    value.glitch_left = djy_tm_get_u32(data + 116);
    value.glitch_right = djy_tm_get_u32(data + 120);
    value.command_rx = djy_tm_get_u32(data + 124);
    value.command_errors = djy_tm_get_u32(data + 128);
    value.can_rx = djy_tm_get_u32(data + 132);
    value.can_errors = djy_tm_get_u32(data + 136);
    value.can_status = djy_tm_get_u32(data + 140);
    value.imu_gyro_ok = djy_tm_get_u32(data + 144);
    value.imu_pkt_bad = djy_tm_get_u32(data + 148);
    value.imu_resync = djy_tm_get_u32(data + 152);
    value.imu_dma_restart = djy_tm_get_u32(data + 156);
    for (unsigned i = 0; i < DJY_TELEMETRY_TIMING_WORDS; ++i)
        value.timing[i] = djy_tm_get_u32(data + 160 + 4u * i);
    if (value.tps_raw > 4095u || value.tps_idle > 4095u || value.tps_pct > 100u ||
        value.sas_raw > 16383u || value.sas_center > 16383u ||
        value.dac_left > 4095u || value.dac_right > 4095u ||
        value.traction_milli > 1000u || value.requested > 100u ||
        value.limit > 100u || value.applied > 100u ||
        (value.flags & ~DJY_TM_FLAGS_MASK) || value.timing_valid > 1u ||
        value.steer_milli < -3142 || value.steer_milli > 3142 ||
        value.kp_milli < 0 || value.ki_milli < 0 || value.kd_milli < 0)
        return false;
    if (value.timing_valid && (value.timing[0] != 1u || value.timing[2] >= 13u ||
        value.timing[8] > 1u || value.timing[12] > 1u || value.timing[13] > 65535u))
        return false;
    *out = value;
    return true;
}

/* Bounded stream assembler. Scan again after corrupt/inserted/missing bytes,
 * including a valid header embedded in a rejected candidate. No heap.
 * After true, bytes holds one complete frame until the next push.
 * errors counts rejected header/CRC candidates, not arbitrary idle noise. */
typedef struct {
    uint8_t bytes[DJY_TELEMETRY_SIZE];
    uint16_t used;
    uint32_t errors;
} DjyTelemetryRx;
static inline void djy_tm_discard_byte(DjyTelemetryRx *rx) {
    --rx->used;
    memmove(rx->bytes, rx->bytes + 1, rx->used);
}
static inline bool djy_telemetry_push(DjyTelemetryRx *rx, uint8_t byte) {
    rx->bytes[rx->used++] = byte;
    while (rx->used >= 2u) {
        if (rx->bytes[0] != 0xd5u || rx->bytes[1] != 0x4au) {
            djy_tm_discard_byte(rx);
            continue;
        }
        if (rx->used < 6u) return false;
        if (!djy_tm_header_valid(rx->bytes)) {
            ++rx->errors;
            djy_tm_discard_byte(rx);
            continue;
        }
        if (rx->used < DJY_TELEMETRY_SIZE) return false;
        if (djy_tm_frame_valid(rx->bytes, rx->used)) {
            rx->used = 0u;
            return true;
        }
        ++rx->errors;
        djy_tm_discard_byte(rx);
    }
    return false;
}
#endif
