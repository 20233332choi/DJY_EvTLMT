#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
typedef struct { uint32_t ErrorCode, RxState; } UART_HandleTypeDef;
typedef struct { uint32_t remaining; } DMA_HandleTypeDef;
UART_HandleTypeDef huart4;
DMA_HandleTypeDef hdma_uart4_rx;
#define HAL_OK 0
#define HAL_UART_ERROR_NONE 0
#define HAL_UART_STATE_READY 0
#define __HAL_DMA_GET_COUNTER(h) ((h)->remaining)
#define __HAL_UART_CLEAR_OREFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_NEFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_FEFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_IDLEFLAG(h) ((void)(h))
#define __HAL_UART_ENABLE_IT(h,x) ((void)(h))
#define UART_IT_IDLE 1
#define UART4_IRQn 1
#define __get_PRIMASK() 0u
#define __disable_irq() ((void)0)
#define __set_PRIMASK(x) ((void)(x))
#define __DMB() ((void)0)
void HAL_NVIC_SetPriority(int irq,int a,int b) {}
void HAL_NVIC_EnableIRQ(int irq) {}
static uint32_t clock_us=100000,received_gyro[4],parsed_gyro[4],received_count;
uint32_t VehicleClock_Us32(void) { return clock_us; }
void Timing_ImuPacket(uint8_t kind,uint32_t rx,uint32_t parsed) {
    if(kind==0x52){CHECK(received_count<4);received_gyro[received_count]=rx;parsed_gyro[received_count++]=parsed;}
}
static uint32_t tick = 100;
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t ms) { tick += ms; }
int HAL_UART_Transmit(UART_HandleTypeDef*h,uint8_t*b,uint16_t n,uint32_t timeout){return 0;}
int HAL_UART_Receive_DMA(UART_HandleTypeDef*h,uint8_t*b,uint16_t n){return 0;}
int HAL_UART_DMAStop(UART_HandleTypeDef*h){return 0;}
int HAL_UART_Abort(UART_HandleTypeDef*h){return 0;}
#include "imu_sensor.c"
#include "torque_vectoring.h"

static void packet(uint8_t type,int16_t x,int16_t y,int16_t z) {
    uint8_t p[11]={0x55,type,(uint8_t)x,(uint8_t)(x>>8),(uint8_t)y,(uint8_t)(y>>8),(uint8_t)z,(uint8_t)(z>>8),0,0,0};
    for(int i=0;i<10;i++) p[10]+=p[i];
    CHECK(parse_packet(p));
}
static void sensor(float yaw,float ay) {
    s_yaw_bias=s_lat_bias=0;
    Deglitch_Reset(&s_yaw_dg); LPF1_Reset(&s_yaw_lpf); LPF1_Reset(&s_acc_lpf);
    // Nonzero X/Y gyro distinguishes Wz from the other axes.
    packet(0x52,1111,-2222,(int16_t)lroundf(yaw/DEG2RAD*32768/2000));
    packet(0x51,100,(int16_t)lroundf(ay/G_ACC*32768/16),2048);
    IMU_Update();
    CHECK(fabsf(IMU_GetYawRate()-yaw)<.0011f);
    CHECK(fabsf(IMU_GetLateralAcc()-ay)<.003f);
    CHECK(IMU_GetAccelerationX()>0 && IMU_GetAccelerationZ()>9.8f);
}
static TV_t run(float ref,float measured) {
    TV_t tv={0}; tv.rpm_left=tv.rpm_right=830; tv.tps_fraction=.5f;
    float v=830.0f/GEAR_RATIO_DEFAULT*2*M_PI*TIRE_RADIUS/60;
    tv.steering_angle_rad=atanf(ref*(WHEELBASE+UNDERSTEER_GRADIENT*v*v)/v);
    sensor(measured,v*measured);
    tv.imu_yaw_rate=IMU_GetYawRate();
    TV_Init(); TV_SetTVEnabled(true); TV_SetStrength(1);
    TV_Update(&tv);
    CHECK(tv.tv_active && !tv.ed_active);
    CHECK(fabsf(tv.desired_yaw-ref)<.0001f);
    CHECK(fabsf(tv.yaw_error-(ref-measured))<.0011f);
    CHECK(tv.traction_scale==1.0f);
    return tv;
}
int main(void) {
    // Two IDLE bursts cross the DMA wrap while main parsing is delayed.
    // Their receive timestamps must retain the original 10 ms spacing.
    s_read_idx=s_idle_write=500;
    for(unsigned burst=0;burst<2;burst++) {
        uint8_t bytes[22]={0};
        for(unsigned p=0;p<2;p++) {
            uint8_t *b=bytes+11*p;b[0]=0x55;b[1]=p?0x52:0x51;b[6]=1;
            for(unsigned j=0;j<10;j++)b[10]+=b[j];
        }
        for(unsigned j=0;j<22;j++)s_rx[(500+burst*22+j)%RX_BUF_SIZE]=bytes[j];
        hdma_uart4_rx.remaining=RX_BUF_SIZE-(500+(burst+1)*22)%RX_BUF_SIZE;
        clock_us=100000+10000*burst;IMU_OnRxIdle();
    }
    clock_us=150000;IMU_ProcessData();
    CHECK(received_count==2 && received_gyro[1]-received_gyro[0]==10000);
    CHECK(received_gyro[0]==99913 && parsed_gyro[0]==150000);
    CHECK(s_read_idx==s_idle_write);
    TV_t tv=run(.4f,.4f); CHECK(fabsf(tv.yaw_error)<.0011f);
    tv=run(-.4f,-.4f); CHECK(fabsf(tv.yaw_error)<.0011f);
    tv=run(.4f,0); CHECK(tv.delta_power>0 && tv.power_right>tv.power_left);
    tv=run(-.4f,0); CHECK(tv.delta_power<0 && tv.power_right<tv.power_left);
    tv=run(.4f,.6f); CHECK(tv.delta_power<0 && tv.power_right<tv.power_left);
    tv=run(-.4f,-.6f); CHECK(tv.delta_power>0 && tv.power_right>tv.power_left);
    puts("FLU sensor, matching yaw, and corrective torque PASS");
}
