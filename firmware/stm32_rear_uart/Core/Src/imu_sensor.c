#include "imu_sensor.h"
#include "board_config.h"
#include "vehicle_params.h"
#include "filters.h"
#include "main.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* CubeMX: USART3 TX=PC10 / RX=PC11 (board_config.h), IMU_UART_BAUD, 8N1,
 * RX는 DMA1 Stream1 Ch4 — 반드시 Circular 모드 */
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart2; /* read-only probe results on ST-Link USB */
extern DMA_HandleTypeDef hdma_usart3_rx;

/* ★256→512로 늘렸다. 메인 루프가 SD f_write+f_sync에서 수십 ms 블로킹될 수
 * 있는데, 그동안 IMU_ProcessData()가 안 돌아서 DMA 링버퍼가 계속 쌓인다.
 * 100Hz×2종류×11바이트 = 2200B/s 이므로 256바이트는 116ms치밖에 안 되어
 * SD 카드가 한 번 느리게 응답하면 링버퍼가 덮여서 파싱 동기가 깨진다.
 * 512바이트면 232ms치 — F446(128KB RAM)에서 256바이트는 공짜나 다름없다. */
#define RX_BUF_SIZE   512u
#define WT_HEADER     0x55u
#define WT_ACC        0x51u   /* 가속도 */
#define WT_GYRO       0x52u   /* 각속도 */
#define WT_PKT_LEN    11u

static uint8_t  s_rx[RX_BUF_SIZE];
static uint16_t s_read_idx = 0;

static volatile float    s_yaw_rate = 0.0f;   /* rad/s, 원본(바이어스·필터 전) */
static volatile float    s_lat_acc  = 0.0f;   /* m/s^2, 원본 */
static volatile float    s_long_acc = 0.0f;  /* m/s^2, 원본 종방향 */
static volatile float    s_raw_ax   = 0.0f;
static volatile float    s_raw_ay   = 0.0f;
static volatile float    s_raw_az   = 0.0f;
static volatile uint32_t s_last_ms  = 0;

/* 제어 주기(100Hz)에 동기해서 갱신되는 필터 출력 — 제어/로깅은 이 값을 쓴다.
 * ★필터를 파서 안(패킷 도착 시점)이 아니라 IMU_Update()에서 도는 이유:
 * 1차 IIR의 alpha는 고정 dt 전제인데, 패킷 도착 주기는 센서 설정이 안 먹거나
 * 통신이 밀리면 흔들린다. 제어 틱에서 돌리면 dt가 정확히 CONTROL_DT로 보장된다. */
static float      s_yaw_filt = 0.0f;
static float      s_acc_filt = 0.0f;
static Deglitch_t s_yaw_dg;
static LPF1_t     s_yaw_lpf;
static LPF1_t     s_acc_lpf;

/* 자이로/가속도계 0점 캘리브레이션 상태 */
#define IMU_CAL_MIN_SAMPLES   20u     /* 이보다 적게 들어오면 캘리브레이션 실패 처리 */
#define IMU_CAL_MAX_BIAS_RAD  0.3f    /* 약 17°/s. 이보다 크면 "정지 상태 아니었음"으로 보고 거부 */
/* ★가속도계 0점: 정지 상태에서도 횡가속도가 0이 아니라 보통 0.1~0.5 m/s^2 정도
 * 나온다. 차가 움직여서가 아니라 (a) IMU가 수평에서 1~2° 기울어져 있으면 중력의
 * sin 성분이 Y축에 실리고(1.2° → 0.2m/s^2), (b) MEMS 자체 zero-g offset이
 * ±20~80mg 있기 때문이다. LAT_ACC_MISMATCH_OK(2.0)에 비하면 작아서 제어에는
 * 영향이 없지만, 로그 판독과 IMU_LAT_ACC_SIGN 부호 확인이 편해지도록 자이로와
 * 같은 구간에서 같이 잡는다.
 * 임계값 1.5m/s^2(≈8.8° 기울기)를 넘으면 "차를 눕혀놨거나 경사로에 세웠음"으로
 * 보고 거부한다 — 진짜 기울기까지 0으로 지워버리면 안 되기 때문. */
#define IMU_CAL_MAX_ACC_BIAS  1.5f
static volatile float    s_yaw_bias   = 0.0f;
static volatile float    s_lat_bias   = 0.0f;
static volatile bool     s_calibrated = false;
static volatile bool     s_cal_active = false;
static volatile double   s_cal_sum    = 0.0;
static volatile uint32_t s_cal_count  = 0;
static volatile double   s_cal_acc_sum   = 0.0;
static volatile uint32_t s_cal_acc_count = 0;

/* 디버그용 진단 카운터 — Live Expression으로 확인 가능 */
static volatile uint32_t s_pkt_ok    = 0;   /* 체크섬 통과 (ACC/GYRO만 여기 도달) */
static volatile uint32_t s_pkt_bad   = 0;   /* 체크섬 실패 */
static volatile uint32_t s_resync    = 0;   /* 헤더 불일치로 1바이트씩 스킵한 횟수 */
static volatile uint32_t s_gyro_ok   = 0;   /* 실제 자이로(0x52) 갱신 횟수 */
static volatile uint32_t s_acc_ok    = 0;   /* 실제 가속도(0x51) 갱신 횟수 */
static volatile uint32_t s_dma_restart = 0; /* 워치독이 DMA를 되살린 횟수 */

static const float DEG2RAD = 3.14159265f / 180.0f;
static const float G_ACC   = 9.80665f;

/* Boot only, before TIM6 starts. Never transmit at an unverified baud rate.
 * A quiet sensor (or one outputting only angles) cannot be identified by this
 * passive test. Failure leaves the existing 115200 receive path available.
 * 1500ms per rate is diagnostic coverage, not proof that a sensor is faulty. */
static bool imu_detect_baud(void) {
    static const uint32_t rates[] = {115200u, 9600u, 19200u, 38400u,
                                     57600u, 4800u, 230400u};
    for (unsigned rate = 0; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
        uint8_t packet[WT_PKT_LEN];
        unsigned used = 0, acc = 0, gyro = 0, bad = 0, errors = 0;
        uint32_t bytes = 0;
        huart3.Init.BaudRate = rates[rate];
        bool ready = HAL_UART_Init(&huart3) == HAL_OK;
        if (ready) {
            __HAL_UART_CLEAR_OREFLAG(&huart3);
            uint32_t start = HAL_GetTick();
            while ((HAL_GetTick() - start) < 1500u && (acc < 2u || gyro < 2u)) {
                uint8_t byte;
                HAL_StatusTypeDef result = HAL_UART_Receive(&huart3, &byte, 1, 10);
                if (result != HAL_OK) {
                    if (result != HAL_TIMEOUT) {
                        ++errors;
                        __HAL_UART_CLEAR_OREFLAG(&huart3);
                    }
                    continue;
                }
                ++bytes;
                packet[used++] = byte;
                if (used < WT_PKT_LEN) continue;
                if (packet[0] == WT_HEADER &&
                    (packet[1] == WT_ACC || packet[1] == WT_GYRO)) {
                    uint8_t sum = 0;
                    for (unsigned i = 0; i < WT_PKT_LEN - 1u; ++i) sum += packet[i];
                    if (sum == packet[WT_PKT_LEN - 1u]) {
                        if (packet[1] == WT_ACC) ++acc; else ++gyro;
                        used = 0;
                        continue;
                    }
                    ++bad;
                }
                memmove(packet, packet + 1, WT_PKT_LEN - 1u);
                used = WT_PKT_LEN - 1u;
            }
        }
        char line[128];
        int n = snprintf(line, sizeof(line),
                         "IMU probe baud=%lu bytes=%lu acc=%u gyro=%u bad=%u err=%u init=%u\r\n",
                         (unsigned long)rates[rate], (unsigned long)bytes,
                         acc, gyro, bad, errors, (unsigned)ready);
        if (n > 0 && n < (int)sizeof(line))
            HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)n, 50);
        if (acc >= 2u && gyro >= 2u) return true;
    }
    huart3.Init.BaudRate = IMU_UART_BAUD;
    if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
    return false;
}

/* WitMotion 설정 프로토콜: FF AA <reg> <dataL> <dataH>
 * ★이전 버전은 reg=0x04(BAUD 레지스터)로 잘못 보내고 있었다. 공식 SDK(REG.h,
 * https://github.com/WITMOTION/WitStandardProtocol_JY901) 확인 결과 정확한
 * 주소는 RRATE=0x03, RSW=0x02, BAUD=0x04 이다. 그동안 RRATE가 실제로는 전혀
 * 바뀌지 않아 공장 기본값(~20Hz)으로 계속 동작했던 것 — 그래서 s_gyro_ok가
 * 20/sec 근처였다. */
static void imu_configure(void) {
    uint8_t cmd_rsw[5]  = { 0xFF, 0xAA, 0x02, 0x06, 0x00 };  /* RSW: ACC|GYRO만 (0x02|0x04) */
    uint8_t cmd_rate[5] = { 0xFF, 0xAA, 0x03, 0x09, 0x00 };  /* RRATE: 100Hz */
    HAL_UART_Transmit(&huart3, cmd_rsw,  sizeof(cmd_rsw),  50);
    HAL_Delay(5);
    HAL_UART_Transmit(&huart3, cmd_rate, sizeof(cmd_rate), 50);
}

void IMU_Init(void) {
    s_read_idx = 0;
    Deglitch_Reset(&s_yaw_dg);
    LPF1_Reset(&s_yaw_lpf);
    LPF1_Reset(&s_acc_lpf);
    s_yaw_filt = 0.0f;
    s_acc_filt = 0.0f;
    bool detected = imu_detect_baud();
    /* Keep the existing 100Hz configuration only at its original baud.
     * At 9600, 100Hz ACC+GYRO exceeds UART capacity. Other detected rates
     * retain the sensor's existing output settings; no BAUD/SAVE command. */
    if (detected && huart3.Init.BaudRate == IMU_UART_BAUD) imu_configure();
    char status[80];
    int n = snprintf(status, sizeof(status), "IMU baud=%lu detected=%u\r\n",
                     (unsigned long)huart3.Init.BaudRate, (unsigned)detected);
    if (n > 0 && n < (int)sizeof(status))
        HAL_UART_Transmit(&huart2, (uint8_t *)status, (uint16_t)n, 50);
    HAL_Delay(50);
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    HAL_UART_Receive_DMA(&huart3, s_rx, RX_BUF_SIZE);
}

/* DMA가 다음에 쓸 위치 */
static uint16_t dma_write_idx(void) {
    return (uint16_t)(RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx));
}

/* 체크섬 통과 시 true, 실패 시 false — 호출부가 이 값으로 이동폭을 결정한다 */
static bool parse_packet(const uint8_t *p) {
    uint8_t sum = 0;
    for (int i = 0; i < 10; i++) sum += p[i];
    if (sum != p[10]) { s_pkt_bad++; return false; }
    s_pkt_ok++;

    if (p[1] == WT_GYRO) {
        /* 0x52: 55 52 wxL wxH wyL wyH wzL wzH TL TH SUM → wz = p[6],p[7] */
        int16_t wz = (int16_t)((uint16_t)p[6] | ((uint16_t)p[7] << 8));
        float dps  = (float)wz / 32768.0f * 2000.0f;   /* °/s */
        s_yaw_rate = dps * DEG2RAD;                     /* rad/s, 원본(bias 미반영) */
        s_last_ms  = HAL_GetTick();
        s_gyro_ok++;
        if (s_cal_active) {          /* 캘리브레이션 중이면 원본값을 누적 */
            s_cal_sum += (double)s_yaw_rate;
            s_cal_count++;
        }
    } else if (p[1] == WT_ACC) {
        /* ★버그 수정: 0x51 패킷 배치는 55 51 AxL AxH AyL AyH AzL AzH TL TH SUM.
         * 즉 p[2],p[3]은 Ax(종방향)이고 Ay(횡방향)는 p[4],p[5]다. 예전 코드는
         * p[2],p[3]을 읽으면서 변수명만 ay로 붙여놔서, 횡가속도 대신 종가속도가
         * traction_scale()에 들어가고 있었다. 그 결과 코너링 중엔 mismatch가
         * 항상 커져서 TV 개입이 부당하게 깎이고, 급가속/급감속 때는 반대로
         * 엉뚱하게 트랙션 상실로 판정됐다. (자이로 wz=p[6],p[7]은 원래 맞음) */
        int16_t ax = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        int16_t ay = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
        int16_t az = (int16_t)((uint16_t)p[6] | ((uint16_t)p[7] << 8));
        s_long_acc = ((float)ax / 32768.0f * 16.0f) * G_ACC;
        float g    = (float)ay / 32768.0f * 16.0f;      /* g */
        s_lat_acc  = g * G_ACC;                          /* m/s^2, 원본(bias 미반영) */
        s_raw_ax   = s_long_acc;
        s_raw_ay   = s_lat_acc;
        s_raw_az   = ((float)az / 32768.0f * 16.0f) * G_ACC;
        s_acc_ok++;
        if (s_cal_active) {          /* 캘리브레이션 중이면 원본값을 누적 */
            s_cal_acc_sum += (double)s_lat_acc;
            s_cal_acc_count++;
        }
    }
    return true;
}

void IMU_ProcessData(void) {
    uint16_t w = dma_write_idx();

    while (s_read_idx != w) {
        uint16_t avail = (w >= s_read_idx) ? (w - s_read_idx)
                                           : (RX_BUF_SIZE - s_read_idx + w);
        if (avail < WT_PKT_LEN) break;

        /* 헤더(0x55) + 관심 타입(ACC/GYRO)까지 같이 확인 — 가짜 헤더/불필요한
         * 타입(Time/Angle/Mag)을 조기에 걸러내 리싱크 낭비를 줄인다. */
        uint8_t byte0 = s_rx[s_read_idx];
        uint8_t byte1 = s_rx[(s_read_idx + 1) % RX_BUF_SIZE];

        if (byte0 != WT_HEADER || (byte1 != WT_ACC && byte1 != WT_GYRO)) {
            s_read_idx = (s_read_idx + 1) % RX_BUF_SIZE;
            s_resync++;
            continue;
        }

        uint8_t pkt[WT_PKT_LEN];
        for (uint16_t i = 0; i < WT_PKT_LEN; i++)
            pkt[i] = s_rx[(s_read_idx + i) % RX_BUF_SIZE];

        if (parse_packet(pkt)) {
            s_read_idx = (s_read_idx + WT_PKT_LEN) % RX_BUF_SIZE;  /* 성공: 11바이트 점프 */
        } else {
            s_read_idx = (s_read_idx + 1) % RX_BUF_SIZE;  /* 실패: 1바이트만 이동해 재정렬 */
        }
    }
}

/* 100Hz 제어 틱에서 1회 호출 — 원본값에 필터를 적용해 제어용 값을 갱신한다.
 *  1) Deglitch : 물리적으로 불가능한 점프(요각가속도 30rad/s^2 초과)만 최대
 *                2틱 무시. 평상시엔 입력을 그대로 통과시키므로 지연이 0이다.
 *                실차에서 자이로에 들어오는 노이즈는 대부분 이런 단발 스파이크다.
 *  2) LPF1     : fc=25Hz(군지연 6.4ms). 차체 진동이 자이로 대역으로 접히는 걸
 *                눌러준다. 100Hz 제어 루프에 6.4ms는 무시할 수준. */
void IMU_Update(void) {
    float raw = s_yaw_rate - s_yaw_bias;
    float dg  = Deglitch_Update(&s_yaw_dg, raw, IMU_YAW_MAX_STEP, DEGLITCH_MAX_REJECT);
    s_yaw_filt = LPF1_Update(&s_yaw_lpf, dg, LPF1_Alpha(IMU_YAW_LPF_FC_HZ, CONTROL_DT));

    /* 횡가속도는 트랙션 판정에만 쓰여서 빠를 필요가 없다 — 더 세게 걸어도 무해 */
    s_acc_filt = LPF1_Update(&s_acc_lpf, s_lat_acc - s_lat_bias,
                             LPF1_Alpha(IMU_ACC_LPF_FC_HZ, CONTROL_DT));
}

float IMU_GetYawRate(void)     { return s_yaw_filt; }
float IMU_GetLateralAcc(void)  { return s_acc_filt; }
float IMU_GetLongitudinalAcc(void) { return s_long_acc; }
float IMU_GetRawAx(void)           { return s_raw_ax; }
float IMU_GetRawAy(void)           { return s_raw_ay; }
float IMU_GetRawAz(void)           { return s_raw_az; }
float IMU_GetYawRateRaw(void)  { return s_yaw_rate - s_yaw_bias; }

bool IMU_IsValid(void) {
    if (s_gyro_ok == 0u) return false; /* no valid data merely because boot tick is small */
    if ((HAL_GetTick() - s_last_ms) > IMU_TIMEOUT_MS) return false;
    /* ★바이어스 보정된 값으로 판정해야 한다. 예전엔 원본 s_yaw_rate를 썼는데,
     * 바이어스가 큰 개체면 정지 상태에서도 이상치로 걸릴 수 있었다. */
    if (fabsf(s_yaw_rate - s_yaw_bias) > IMU_YAW_RATE_MAX) return false;
    return true;
}

/* ★실차 노이즈 대응: UART 프레이밍/오버런 에러나 커넥터 순간 단선으로 DMA가
 * 멈추면 IMU가 영구히 죽는다(이 프로젝트는 USART3_IRQn을 안 쓰므로 HAL의 에러
 * 콜백도 안 온다). 새 자이로 패킷이 한동안 없으면 UART/DMA를 통째로 재시작해
 * 스스로 복구한다. 메인 루프에서 주기적으로 호출할 것. */
void IMU_Watchdog(void) {
    static uint32_t last_try = 0;
    uint32_t now = HAL_GetTick();

    if ((now - s_last_ms) <= IMU_DMA_RESTART_MS) return;
    if ((now - last_try)  <  IMU_DMA_RESTART_MS) return;   /* 재시작 폭주 방지 */
    last_try = now;

    HAL_UART_DMAStop(&huart3);
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    __HAL_UART_CLEAR_NEFLAG(&huart3);
    __HAL_UART_CLEAR_FEFLAG(&huart3);
    huart3.ErrorCode = HAL_UART_ERROR_NONE;

    s_read_idx = 0;
    if (HAL_UART_Receive_DMA(&huart3, s_rx, RX_BUF_SIZE) == HAL_OK) {
        s_dma_restart++;
    }
}

/* 정지 상태에서 duration_ms 동안 자이로 원본값을 평균내어 0점 바이어스로 저장.
 * 블로킹 함수 — 부팅 시 IMU_Init() 직후, 제어 루프 시작 전에 한 번만 호출할 것.
 * 샘플이 너무 적거나(센서 미연결 등) 평균이 비정상적으로 크면(차량이 실제로
 * 움직이고 있었던 경우 등) 캘리브레이션을 거부하고 바이어스=0으로 남긴다. */
void IMU_Calibrate(uint32_t duration_ms) {
    s_cal_sum       = 0.0;
    s_cal_count     = 0;
    s_cal_acc_sum   = 0.0;
    s_cal_acc_count = 0;
    s_yaw_bias      = 0.0f;
    s_lat_bias      = 0.0f;
    s_calibrated    = false;
    s_cal_active    = true;

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < duration_ms) {
        IMU_ProcessData();
    }
    s_cal_active = false;

    if (s_cal_count >= IMU_CAL_MIN_SAMPLES) {
        float bias = (float)(s_cal_sum / (double)s_cal_count);
        if (fabsf(bias) <= IMU_CAL_MAX_BIAS_RAD) {   /* 비정상적으로 크면 거부, bias=0 유지 */
            s_yaw_bias   = bias;
            s_calibrated = true;
        }
    }
    /* 가속도계 0점 — 자이로와 독립적으로 판정한다(자이로가 실패해도 이건 유효할 수 있다) */
    if (s_cal_acc_count >= IMU_CAL_MIN_SAMPLES) {
        float abias = (float)(s_cal_acc_sum / (double)s_cal_acc_count);
        if (fabsf(abias) <= IMU_CAL_MAX_ACC_BIAS) s_lat_bias = abias;
    }

    /* 필터 상태를 캘리브레이션 직후 값으로 프라이밍 — 제어 루프 첫 틱에서
     * 0에서 튀어오르지 않게 하고, 부팅 직후 IMU_GetYawRate()도 곧바로 유효해진다. */
    Deglitch_Reset(&s_yaw_dg);
    LPF1_Reset(&s_yaw_lpf);
    LPF1_Reset(&s_acc_lpf);
    IMU_Update();
}

bool IMU_IsCalibrated(void) { return s_calibrated; }
uint32_t IMU_GetPacketOkCount(void) { return s_pkt_ok; }
uint32_t IMU_GetPacketBadCount(void) { return s_pkt_bad; }
uint32_t IMU_GetResyncCount(void) { return s_resync; }
uint32_t IMU_GetGyroPacketCount(void) { return s_gyro_ok; }
uint32_t IMU_GetAccPacketCount(void) { return s_acc_ok; }
