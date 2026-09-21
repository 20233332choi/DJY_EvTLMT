#include "safety_monitor.h"
#include "can_comm.h"
#include "can_messages.h"
#include "imu_sensor.h"
#include "rpm_sensor.h"
#include "vehicle_params.h"

static FaultCode    s_fault  = FAULT_NONE;
static SafeAction_t s_action = SAFE_ACTION_NONE;

/* 폴트 디바운스 카운터 — 실차 노이즈로 한 틱 튄 걸 폴트로 확정하지 않는다.
 * N틱 연속 이상해야 확정하고, 정상값이 하나라도 들어오면 즉시 리셋한다. */
static uint8_t s_tps_bad = 0;
static uint8_t s_imu_bad = 0;

void Safety_Init(void) {
    s_fault  = FAULT_NONE;
    s_action = SAFE_ACTION_NONE;
    s_tps_bad = 0;
    s_imu_bad = 0;
}

void Safety_Update(void) {
    SensorData_t s = CAN_GetSensorData();

    /* 우선순위: STOP > DISABLE_DIFF > DISABLE_TV. STOP 조건 먼저 검사. */

    /* 1) CAN(Board A) 타임아웃 → 즉시 정지
     *    (이미 100ms 시간 기준이라 별도 디바운스 불필요) */
    if (!CAN_IsSensorFresh()) {
        s_fault = FAULT_CAN_TIMEOUT; s_action = SAFE_ACTION_STOP; return;
    }

    /* 2) TPS 범위 이상(단선/단락) → 즉시 정지
     *    ★MARGIN 추가: 예전엔 TPS_ADC_MIN/MAX를 마진 없이 그대로 비교해서,
     *    idle(=정확히 TPS_ADC_MIN)에서 ADC 노이즈가 1LSB만 아래로 튀어도
     *    즉시 STOP이 걸렸다. 실차에서는 확실히 터졌을 문제다. 여기에
     *    N틱 연속 확인(디바운스)까지 더해 이중으로 막는다. */
    if (s.tps_raw < (TPS_ADC_MIN - TPS_ADC_MARGIN) ||
        s.tps_raw > (TPS_ADC_MAX + TPS_ADC_MARGIN)) {
        if (++s_tps_bad >= FAULT_DEBOUNCE_TPS) {
            s_tps_bad = FAULT_DEBOUNCE_TPS;   /* 오버플로 방지 */
            s_fault = FAULT_TPS_RANGE; s_action = SAFE_ACTION_STOP; return;
        }
    } else {
        s_tps_bad = 0;
    }

    /* 3) SAS(조향각 센서) 이상 → TV와 ED 둘 다 끄고 균등 분배
     *    ★예전엔 Board A가 하트비트로 보내주는 SAS_ERR 비트를 받아만 두고
     *    아무도 안 봤다. 조향각을 못 믿으면 ED(개루프)는 아예 근거가 없고
     *    TV의 목표요레이트도 엉터리가 되므로 차동 자체를 포기해야 한다.
     *    하트비트가 끊겼으면(=비트값이 오래된 값이면) 판단 근거가 없으므로
     *    이 검사는 건너뛴다 — Board A가 통째로 죽은 경우는 위 1)이 잡는다. */
    if (CAN_IsHeartbeatFresh() &&
        (CAN_GetHeartbeatStatus() & HB_STATUS_SAS_ERR)) {
        s_fault = FAULT_SAS_ERROR; s_action = SAFE_ACTION_DISABLE_DIFF; return;
    }

    /* 4) IMU 무효/이상치 → TV만 비활성. ED는 IMU를 안 쓰므로 계속 동작한다. */
    if (!IMU_IsValid()) {
        if (++s_imu_bad >= FAULT_DEBOUNCE_IMU) {
            s_imu_bad = FAULT_DEBOUNCE_IMU;
            s_fault = FAULT_IMU_INVALID; s_action = SAFE_ACTION_DISABLE_TV; return;
        }
    } else {
        s_imu_bad = 0;
    }

    /* 5) RPM 타임아웃(TIM3 Input Capture) → TV 비활성. ED는 차속이 필요없다.
     *    정차 중에는 펄스가 안 오므로 여기가 정상적으로 걸린다(폴트가 아니라
     *    "TV 조건 미충족" 상태에 가깝다). ED가 그 구간을 담당한다. */
    if (!RPM_IsFresh()) {
        s_fault = FAULT_RPM_TIMEOUT; s_action = SAFE_ACTION_DISABLE_TV; return;
    }

    s_fault = FAULT_NONE; s_action = SAFE_ACTION_NONE;
}

FaultCode    Safety_GetFaultCode(void) { return s_fault; }
SafeAction_t Safety_GetAction(void)    { return s_action; }
