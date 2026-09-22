#ifndef CAN_COMM_B_H
#define CAN_COMM_B_H
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint16_t sas_angle; uint16_t tps_raw; } SensorData_t;
typedef struct { uint16_t left; uint16_t right; } MotorRPM_t;
typedef struct {
    uint32_t front_us, received_us;
    uint16_t span_us, sequence;
    bool timestamped;
} SensorTiming_t;
SensorTiming_t CAN_GetSensorTiming(void);

void         CAN_Init(void);
SensorData_t CAN_GetSensorData(void);
MotorRPM_t   CAN_GetMotorRPM(void);
uint8_t      CAN_GetHeartbeatStatus(void);
bool         CAN_IsSensorFresh(void);
/* TV on/off 토글 스위치 상태 (Board A가 0x100 바이트4로 보냄).
 * 디바운스는 송신측에서 끝난 값이고, 프레임이 stale이면 false를 반환한다. */
bool         CAN_IsTVSwitchOn(void);
bool         CAN_IsHeartbeatFresh(void);   /* 상태 비트를 신뢰해도 되는지 */
bool         CAN_IsRpmFresh(void);

/* ★TEMP 진단 도구: 컨트롤러 CAN H/L의 비트레이트/ID 레이아웃을 모르는 상태에서
 * 스니핑하기 위한 함수. main.c의 CAN_SNIFF_MODE=1일 때만 호출된다.
 * 블로킹이며 리턴하지 않는다 — 후보 비트레이트를 순서대로 시도해 USART2로
 * 결과를 출력한 뒤, 가장 프레임이 많이 잡힌 비트레이트로 고정해 계속 덤프한다.
 * ★반드시 Board A와 물리적으로 분리한 상태(별도 배선)에서만 쓸 것 — 이 함수가
 * CAN1을 재초기화하면서 Board A와의 정상 통신을 깬다. */
void         CAN_SniffSweep(void);

/* ★TEMP 진단 카운터 — USART2 출력의 can=rx/err/esr 필드.
 *   rx  = 수신 프레임 총수(ID 무관). 0 = 버스가 완전히 조용함(배선/전원/상대 보드)
 *   err = 버스 레벨 에러 횟수. 증가 = 신호는 오는데 깨짐(종단/비트레이트/H-L 반전)
 *   esr = 마지막 에러의 ESR. LEC(비트 6:4)로 에러 종류 구분 */
extern volatile uint32_t g_can_rx_count;
extern volatile uint32_t g_can_err_count;
extern volatile uint32_t g_can_last_esr;

#endif /* CAN_COMM_B_H */
