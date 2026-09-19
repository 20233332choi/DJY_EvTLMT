#ifndef CAN_MESSAGES_H
#define CAN_MESSAGES_H
#include <stdint.h>

#define CAN_ID_SENSOR_DATA  0x100u   /* A→B: SAS + TPS, 100Hz */
#define CAN_ID_LEFT_RPM     0x200u   /* 좌 ND72680B→B */
#define CAN_ID_RIGHT_RPM    0x201u   /* 우 ND72680B→B */
#define CAN_ID_HEARTBEAT    0x300u   /* A→B: 상태, 10Hz */

/* 0x100 (A→B) 바이트 배치
 *   [0..1] SAS raw (14비트)
 *   [2..3] TPS raw (12비트)
 *   [4]    플래그 — SENSOR_FLAG_* 비트
 * ★DLC를 4→5로 늘렸다(TV 스위치를 Board A로 옮기면서). 두 보드를 반드시
 *  같이 플래시할 것. 한쪽만 구버전이면 수신측 DLC가 4라 플래그가 없는데,
 *  Board B는 그 경우 플래그=0(TV OFF)으로 처리해 안전하게 떨어진다. */
#define CAN_DLC_SENSOR_DATA 5u
#define CAN_DLC_HEARTBEAT   1u

/* 0x100 바이트4 플래그 비트
 * ★TV on/off 토글 스위치는 Board A(운전석 쪽)에서 읽어 여기 실어 보낸다.
 *  Board B의 PC13에서 옮겨온 것 — 배선이 조종석에 가까워 유리하다.
 *  Board B는 이 프레임이 stale이면 무조건 OFF로 본다(= ED로 폴백). */
#define SENSOR_FLAG_TV_SW   0x01u

/* Heartbeat 상태 비트 */
#define HB_STATUS_OK        0x00u
#define HB_STATUS_SAS_ERR   0x01u
#define HB_STATUS_TPS_ERR   0x02u

/* little-endian 패킹/언패킹 */
static inline void can_pack_u16(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline uint16_t can_unpack_u16(const uint8_t *buf) {
    return (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
}

#endif /* CAN_MESSAGES_H */
