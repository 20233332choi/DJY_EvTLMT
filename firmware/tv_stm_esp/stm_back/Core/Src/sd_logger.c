#include "sd_logger.h"
#include "board_config.h"
#include "vehicle_params.h"
#include "imu_sensor.h"
#include "main.h"
#include "fatfs.h"      /* CubeMX FATFS(User-defined) — SPI2(PB13/14/15), CS=PB12 */
#include <stdio.h>
#include <string.h>

/* CubeMX FATFS를 "User-defined" 드라이버로 생성하면 볼륨명이 기본값 USER가 되어
 * fatfs.h에 SDFatFS/SDPath가 아니라 USERFatFS/USERPath로 선언된다. */

#define LOG_BUF_SIZE   512u

/* 더블 버퍼: ISR은 write 버퍼에 적재, 메인은 ready 버퍼를 SD에 기록 */
static char     s_buf[2][LOG_BUF_SIZE];
static volatile uint16_t s_len[2]   = {0, 0};
static volatile uint8_t  s_active   = 0;   /* ISR이 쓰는 버퍼 */
static volatile uint8_t  s_ready    = 0;   /* 0=없음, 1=버퍼0, 2=버퍼1 */
static volatile uint8_t  s_overflow = 0;

static FIL  s_file;
static bool s_open = false;

void SD_Logger_Init(void) {
    if (f_mount(&USERFatFS, USERPath, 1) != FR_OK) return;
    if (f_open(&s_file, "tvlog.csv", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
    const char *hdr =
      "t_ms,sas,tps_pct,rpm_l,rpm_r,yaw,lat,des_yaw,yaw_err,"
      "dP,P_l,P_r,dac_l,dac_r,tv,fault,speed,traction\r\n";
    UINT bw; f_write(&s_file, hdr, strlen(hdr), &bw);
    f_sync(&s_file);
    s_open = true;
}

/* ISR 컨텍스트: snprintf로 한 줄 만들어 active 버퍼에 적재 */
void SD_Logger_Write(const TV_t *tv, SafeAction_t action) {
    if (!s_open) return;
    char line[176];
    int n = snprintf(line, sizeof(line),
        "%lu,%u,%.1f,%u,%u,%.3f,%.2f,%.3f,%.3f,%.2f,%.2f,%.2f,%u,%u,%d,%d,%.2f,%.2f\r\n",
        (unsigned long)HAL_GetTick(),
        0u,                       /* sas: 필요시 tv에 추가 */
        (double)(tv->tps_fraction * 100.0f),
        tv->rpm_left, tv->rpm_right,
        (double)tv->imu_yaw_rate, (double)IMU_GetLateralAcc(),
        (double)tv->desired_yaw, (double)tv->yaw_error,
        (double)tv->delta_power, (double)tv->power_left, (double)tv->power_right,
        tv->dac_left, tv->dac_right,
        tv->tv_active ? 1 : 0, (int)action,
        (double)tv->vehicle_speed, (double)tv->traction_scale);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    uint8_t a = s_active;
    if (s_len[a] + n >= LOG_BUF_SIZE) {
        /* 버퍼 가득 → ready로 넘기고 버퍼 스왑 */
        if (s_ready != 0) { s_overflow = 1; return; }  /* 메인이 못 비웠음 */
        s_ready  = a + 1;
        s_active = a ^ 1;
        a = s_active;
        s_len[a] = 0;
    }
    memcpy(&s_buf[a][s_len[a]], line, n);
    s_len[a] += n;
}

/* 메인 루프: ready 버퍼를 SD에 기록 */
void SD_Logger_Flush(void) {
    if (!s_open || s_ready == 0) return;
    uint8_t idx = s_ready - 1;
    UINT bw;
    f_write(&s_file, s_buf[idx], s_len[idx], &bw);
    f_sync(&s_file);
    s_len[idx] = 0;
    s_ready = 0;
}

void SD_Logger_Close(void) {
    if (!s_open) return;
    SD_Logger_Flush();
    f_close(&s_file);
    s_open = false;
}
