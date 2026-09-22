#include "can_comm.h"
#include "can_messages.h"
#include "board_config.h"
#include "vehicle_params.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include "board_time_sync.h"
#include "vehicle_clock.h"
#include "sensor_time_pair.h"

/* CubeMX: CAN1 RX=PA11 / TX=PA12 (board_config.h), 500kbps, RX FIFO0 인터럽트 */
extern CAN_HandleTypeDef hcan1;
extern UART_HandleTypeDef huart2;

static volatile SensorData_t s_sensor = {0};
static volatile MotorRPM_t   s_rpm    = {0};
static volatile uint8_t      s_hb      = 0;
/* ★TV on/off 토글 스위치 — Board B의 PC13에서 Board A로 옮겼다(운전석 배선).
 * 0x100 프레임 바이트4로 매 틱(100Hz) 들어온다. 디바운스는 송신측에서 이미
 * 끝내서 보내므로 여기서 또 할 필요가 없다. */
static volatile bool         s_tv_switch = false;
static volatile uint32_t     s_t_sensor = 0, s_t_left = 0, s_t_right = 0, s_t_hb = 0;
static SensorTimePair sensor_pair;
static volatile SensorTiming_t sensor_timing;
SensorTiming_t CAN_GetSensorTiming(void) { return sensor_timing; }

/* ★TEMP 스니퍼: Fardriver ND72680B가 실제 어떤 CAN ID/바이트에 RPM을 싣는지
 * 모르므로, 들어오는 모든 ID의 최신 프레임을 슬롯에 담아둔다. Live Expression에
 * s_sniff를 걸고 모터를 돌려보며 어떤 슬롯의 어떤 바이트가 회전속도에 비례해
 * 바뀌는지 눈으로 찾는 용도. 원인 파악되면 이 블록과 sniff_record() 호출 삭제. */
#define SNIFF_SLOTS 12
typedef struct {
    uint32_t id;
    bool     ext;      /* true=확장(29bit) ID, false=표준(11bit) ID */
    uint8_t  len;
    uint8_t  data[8];
    uint32_t count;
    uint32_t last_ms;
} SniffSlot_t;
static volatile SniffSlot_t s_sniff[SNIFF_SLOTS] = {0};

/* ★같은 숫자 ID라도 표준/확장 프레임은 버스 상에서 별개이므로 ext까지 같이
 * 비교해야 한다 (컨트롤러 CAN 스니핑용 — 매뉴얼상 확장 프레임 설정도 가능). */
static void sniff_record(uint32_t id, bool ext, const uint8_t *d, uint8_t len, uint32_t now) {
    int free_slot = -1;
    for (int i = 0; i < SNIFF_SLOTS; i++) {
        if (s_sniff[i].count != 0 && s_sniff[i].id == id && s_sniff[i].ext == ext) {
            s_sniff[i].len = len;
            for (int k = 0; k < len && k < 8; k++) s_sniff[i].data[k] = d[k];
            s_sniff[i].last_ms = now;
            s_sniff[i].count++;
            return;
        }
        if (s_sniff[i].count == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        s_sniff[free_slot].id  = id;
        s_sniff[free_slot].ext = ext;
        s_sniff[free_slot].len = len;
        for (int k = 0; k < len && k < 8; k++) s_sniff[free_slot].data[k] = d[k];
        s_sniff[free_slot].last_ms = now;
        s_sniff[free_slot].count = 1;
    }
}

/* ★TEMP: 버스에 뭔가 오다가 깨지는 건지, 아예 무신호인지 구분하기 위한
 * 카운터. USART2 디버그 출력의 can=rx/err/esr 필드로 나온다.
 *   rx  = 수신한 프레임 총수(ID 무관). 0이면 버스가 완전히 조용하다.
 *   err = 버스 레벨 에러 횟수. 신호는 오는데 깨지면 이쪽이 올라간다.
 *   esr = 마지막 에러의 ESR 레지스터(LEC 필드로 에러 종류 식별). */
volatile uint32_t g_can_rx_count  = 0;
volatile uint32_t g_can_err_count = 0;
volatile uint32_t g_can_last_esr  = 0;

void CAN_Init(void) {
    /* 0x100, 0x200, 0x201, 0x300 수신. 단순화를 위해 전수락 후 콜백에서 분기. */
    CAN_FilterTypeDef f = {0};
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh = 0; f.FilterIdLow = 0;
    f.FilterMaskIdHigh = 0; f.FilterMaskIdLow = 0;   /* mask=0 → 전수락 */
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(&hcan1, &f);

    /* ★자동 Bus-Off 복구(ABOM) 활성화 — 실차 노이즈 대응의 핵심.
     * CubeMX 생성 코드는 AutoBusOff=DISABLE이라, 노이즈로 에러 카운터가
     * 255를 넘어 한 번 Bus-Off에 빠지면 CAN이 영구히 죽고 리셋 전까지
     * 안 돌아온다(= Board A 신호 끊김 → 상시 STOP). ABOM을 켜두면
     * 하드웨어가 128×11 연속 리세시브 비트를 관측한 뒤 자동 복귀한다.
     * ABOM은 초기화 모드에서만 쓸 수 있는데, HAL_CAN_Init()은 초기화 모드를
     * 유지한 채 리턴하고 HAL_CAN_Start()에서 노멀 모드로 전환하므로
     * 정확히 이 사이에서 세팅해야 한다. CubeMX 재생성에도 안 지워진다. */
    SET_BIT(hcan1.Instance->MCR, CAN_MCR_ABOM);
    hcan1.Init.AutoBusOff = ENABLE;   /* HAL 내부 상태와 일치시킴 */

    HAL_CAN_Start(&hcan1);
    BoardTimeSync_Init(true);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_ERROR | CAN_IT_BUSOFF |
                                          CAN_IT_LAST_ERROR_CODE);
}

/* ★TEMP: 버스 레벨 에러(폼/스터프/ACK/CRC 에러, Bus-off 등) 발생 시 호출됨.
 * 뭔가 신호는 오는데 깨지는 상황이면 이 카운터가 계속 늘어난다. */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan) {
    g_can_err_count++;
    g_can_last_esr = hcan->Instance->ESR;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    uint32_t received_us=VehicleClock_Us32();
    CAN_RxHeaderTypeDef h;
    uint8_t d[8];
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &h, d) != HAL_OK) return;
    uint32_t now = HAL_GetTick();
    g_can_rx_count++;   /* ★ID 무관 총 수신 프레임 수 — 버스가 조용한지 판별용 */

    bool     ext = (h.IDE == CAN_ID_EXT);
    uint32_t id  = ext ? h.ExtId : h.StdId;
    sniff_record(id, ext, d, (uint8_t)h.DLC, now);   /* ★TEMP: RPM/컨트롤러 프레임 위치 찾는 중 */
    if(ext || h.RTR!=CAN_RTR_DATA)return;
    if(BoardTimeSync_OnCan(id,d,(uint8_t)h.DLC,received_us))return;
    if(id==TS_ID_SENSOR_TIME || (id==CAN_ID_SENSOR_DATA && h.DLC==8u && d[7]==TS_SENSOR_MARKER)) {
        if(SensorTimePair_Push(&sensor_pair,id,d,(uint8_t)h.DLC,received_us)) {
            /* TIM6 has higher priority than CAN RX. Publish values and their
             * timestamp together so a control tick cannot observe half a pair. */
            uint32_t mask=__get_PRIMASK();__disable_irq();
            s_sensor.sas_angle=can_unpack_u16(sensor_pair.data)&0x3fffu;
            s_sensor.tps_raw=can_unpack_u16(sensor_pair.data+2)&0xfffu;
            s_tv_switch=(sensor_pair.data[4]&SENSOR_FLAG_TV_SW)!=0;
            sensor_timing=(SensorTiming_t){sensor_pair.front_us,received_us,
                                          sensor_pair.span,sensor_pair.data_seq,true};
            s_t_sensor=now;
            __set_PRIMASK(mask);
        }
        return;
    }

    switch (h.StdId) {
        case CAN_ID_SENSOR_DATA:
            if(h.DLC<4u)return;
            sensor_timing=(SensorTiming_t){0,received_us,0,0,false};
            /* ★DLC가 5 미만이면 구버전 Board A다 — 플래그 바이트가 아예 없으므로
             * 0(TV OFF)으로 둔다. 두 보드를 따로 플래시했을 때 쓰레기값을 읽고
             * TV가 멋대로 켜지는 걸 막는 방어다. */
            s_tv_switch = (h.DLC >= 5u) && ((d[4] & SENSOR_FLAG_TV_SW) != 0u);
            s_sensor.sas_angle = can_unpack_u16(&d[0]) & 0x3FFFu;
            s_sensor.tps_raw   = can_unpack_u16(&d[2]) & 0x0FFFu;
            s_t_sensor = now;
            break;
        case CAN_ID_LEFT_RPM:
            s_rpm.left = can_unpack_u16(&d[0]);   /* rpm = raw */
            s_t_left   = now;
            break;
        case CAN_ID_RIGHT_RPM:
            s_rpm.right = can_unpack_u16(&d[0]);  /* rpm = raw */
            s_t_right   = now;
            break;
        case CAN_ID_HEARTBEAT:
            s_hb   = d[0];
            s_t_hb = now;
            break;
        default: break;
    }
}

SensorData_t CAN_GetSensorData(void) {
    SensorData_t s; s.sas_angle = s_sensor.sas_angle; s.tps_raw = s_sensor.tps_raw;
    return s;
}
MotorRPM_t CAN_GetMotorRPM(void) {
    MotorRPM_t m; m.left = s_rpm.left; m.right = s_rpm.right; return m;
}
uint8_t CAN_GetHeartbeatStatus(void) { return s_hb; }

bool CAN_IsSensorFresh(void) {
    return (HAL_GetTick() - s_t_sensor) <= CAN_TIMEOUT_MS;
}

/* ★프레임이 stale이면 무조건 OFF로 본다. 통신이 끊긴 상태에서 마지막으로
 * 받은 ON을 계속 붙들고 있으면, 운전자가 스위치를 내려도 TV가 안 꺼진다.
 * OFF로 떨어지면 ED(개루프, IMU 불필요)가 대신 동작하므로 안전한 폴백이다. */
bool CAN_IsTVSwitchOn(void) {
    return s_tv_switch && CAN_IsSensorFresh();
}
/* 하트비트(10Hz)의 상태 비트를 믿어도 되는지 — 오래된 비트로 SAS 폴트를
 * 판정하면 이미 복구된 고장이 계속 남아있게 된다. */
bool CAN_IsHeartbeatFresh(void) {
    return (s_t_hb != 0) && ((HAL_GetTick() - s_t_hb) <= HB_TIMEOUT_MS);
}
bool CAN_IsRpmFresh(void) {
    /* Safety_Update()는 이제 RPM_IsFresh()(TIM3 Input Capture)를 쓴다.
     * 이 함수는 CAN으로도 RPM을 받을 경우를 대비해 원래 로직만 유지. */
    uint32_t now = HAL_GetTick();
    return ((now - s_t_left)  <= RPM_TIMEOUT_MS) &&
           ((now - s_t_right) <= RPM_TIMEOUT_MS);
}

/* =====================================================================
 *  ★TEMP: 컨트롤러 CAN H/L 스니핑 도구 (CAN_SNIFF_MODE 전용)
 * ---------------------------------------------------------------------
 *  컨트롤러의 실제 CAN 비트레이트/ID 레이아웃을 몰라서, 후보 비트레이트를
 *  순서대로 시도하며 SILENT(리슨온리) 모드로 엿듣는다. SILENT 모드는 ACK나
 *  에러프레임을 절대 내보내지 않으므로, 비트레이트가 틀려도 컨트롤러의
 *  CAN 버스에 어떤 영향도 주지 않는다 — 미지의 버스를 안전하게 훔쳐볼 수 있다.
 *
 *  ★★ 반드시 Board A와 물리적으로 분리한 상태에서만 켤 것. 이 함수는 hcan1을
 *  계속 재초기화하며 Board A와의 정상 통신을 깨고, main()에서 이 함수가
 *  리턴하지 않으므로 평소 동작(IMU/RPM/DAC/TV/로깅)은 전혀 돌지 않는다.
 *
 *  결선: 컨트롤러 CAN H/L은 STM32 PA11/PA12에 직결하는 게 아니라, Board B가
 *  Board A와 통신할 때 쓰는 것과 같은 CAN 트랜시버 모듈의 CANH/CANL 단자에
 *  물려야 한다(PA11/PA12는 트랜시버 앞단 디지털 RX/TX). 트랜시버는 그대로 두고
 *  버스 쪽 배선만 Board A에서 컨트롤러로 바꿔 물리면 된다.
 * ===================================================================== */
typedef struct { uint16_t presc; uint32_t baud; } BaudCandidate_t;
/* TimeSeg1=10TQ+TimeSeg2=3TQ+동기1TQ=14TQ 고정(샘플포인트 71.4%, 기존 500k
 * 설정과 동일 비율) — Prescaler만 바꿔 비트레이트를 바꾼다.
 * baud = PCLK1(42MHz) / (Prescaler × 14) */
static const BaudCandidate_t k_baud_candidates[] = {
    {  6,  500000u },   /* Board A/B와 같은 값 — 가장 유력한 후보 */
    {  3, 1000000u },
    { 12,  250000u },
    { 24,  125000u },
};
#define BAUD_CANDIDATE_COUNT (sizeof(k_baud_candidates) / sizeof(k_baud_candidates[0]))

static void sniff_uart(const char *s) {
    HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 100);
}

static void sniff_reinit_can(uint16_t prescaler) {
    HAL_CAN_Stop(&hcan1);
    HAL_CAN_DeInit(&hcan1);

    hcan1.Init.Prescaler           = prescaler;
    hcan1.Init.Mode                = CAN_MODE_SILENT;   /* 리슨온리 — 절대 송신 안 함 */
    hcan1.Init.SyncJumpWidth       = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1            = CAN_BS1_10TQ;
    hcan1.Init.TimeSeg2            = CAN_BS2_3TQ;
    hcan1.Init.TimeTriggeredMode   = DISABLE;
    hcan1.Init.AutoBusOff          = DISABLE;
    hcan1.Init.AutoWakeUp          = DISABLE;
    hcan1.Init.AutoRetransmission  = DISABLE;
    hcan1.Init.ReceiveFifoLocked   = DISABLE;
    hcan1.Init.TransmitFifoPriority= DISABLE;
    HAL_CAN_Init(&hcan1);

    CAN_FilterTypeDef f = {0};
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh = 0; f.FilterIdLow = 0;
    f.FilterMaskIdHigh = 0; f.FilterMaskIdLow = 0;   /* 전수락 — 표준/확장 프레임 다 받음 */
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(&hcan1, &f);

    HAL_CAN_Start(&hcan1);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_ERROR | CAN_IT_BUSOFF |
                                          CAN_IT_LAST_ERROR_CODE);
}

static void sniff_clear(void) {
    for (int i = 0; i < SNIFF_SLOTS; i++) s_sniff[i].count = 0;
    g_can_err_count = 0;
}

static uint32_t sniff_total_frames(void) {
    uint32_t sum = 0;
    for (int i = 0; i < SNIFF_SLOTS; i++) sum += s_sniff[i].count;
    return sum;
}

static void sniff_print_table(void) {
    char line[96];
    for (int i = 0; i < SNIFF_SLOTS; i++) {
        if (s_sniff[i].count == 0) continue;
        int n = snprintf(line, sizeof(line),
            "  %s 0x%08lX len=%u cnt=%lu  %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            s_sniff[i].ext ? "EXT" : "STD",
            (unsigned long)s_sniff[i].id, s_sniff[i].len, (unsigned long)s_sniff[i].count,
            s_sniff[i].data[0], s_sniff[i].data[1], s_sniff[i].data[2], s_sniff[i].data[3],
            s_sniff[i].data[4], s_sniff[i].data[5], s_sniff[i].data[6], s_sniff[i].data[7]);
        if (n > 0) sniff_uart(line);
    }
}

/* 블로킹, 리턴하지 않음. USART2(115200)로 결과를 출력한다.
 * 1) 후보 비트레이트를 1초씩 돌며 프레임/에러 수를 센다
 * 2) 에러 없이 프레임이 가장 많이 잡힌 비트레이트로 고정
 * 3) 그 비트레이트로 계속 스니핑 테이블 전체를 0.5초마다 찍는다
 *    — 이 상태에서 스로틀을 움직이거나 모터를 돌려보며 어떤 ID의 어떤
 *      바이트가 반응하는지 눈으로 대조할 것 (RPM/전류/전압 등, 13.3.4 참고) */
void CAN_SniffSweep(void) {
    sniff_uart("\r\n=== CAN bitrate sweep start ===\r\n");

    uint32_t best_baud   = 0;
    uint32_t best_frames = 0;
    uint16_t best_presc  = 0;

    for (unsigned i = 0; i < BAUD_CANDIDATE_COUNT; i++) {
        sniff_reinit_can(k_baud_candidates[i].presc);
        sniff_clear();
        HAL_Delay(1000);   /* 1초간 수신 */

        uint32_t frames = sniff_total_frames();
        uint32_t errs    = g_can_err_count;

        char line[96];
        int n = snprintf(line, sizeof(line),
            "%8lu bps : frames=%-6lu errors=%-6lu%s\r\n",
            (unsigned long)k_baud_candidates[i].baud,
            (unsigned long)frames, (unsigned long)errs,
            (frames > 0 && errs == 0) ? "   <-- 후보" : "");
        if (n > 0) sniff_uart(line);

        if (frames > best_frames && errs == 0) {
            best_frames = frames;
            best_baud   = k_baud_candidates[i].baud;
            best_presc  = k_baud_candidates[i].presc;
        }
    }

    if (best_baud == 0) {
        sniff_uart("=== 어떤 비트레이트에서도 유효 프레임 없음. CAN H/L 결선(트랜시버 경유"
                    "여부), 종단저항(120옴), 매뉴얼 6.1.5 CAN 활성화 설정을 확인할 것 ===\r\n");
        while (1) { HAL_Delay(1000); }   /* 재부팅 전까지 여기서 정지 */
    }

    {
        char line[80];
        int n = snprintf(line, sizeof(line),
            "=== 확정: %lu bps. 이후 신규 프레임을 계속 덤프합니다 ===\r\n",
            (unsigned long)best_baud);
        if (n > 0) sniff_uart(line);
    }

    sniff_reinit_can(best_presc);
    sniff_clear();

    while (1) {
        HAL_Delay(500);
        sniff_uart("---\r\n");
        sniff_print_table();
    }
}
