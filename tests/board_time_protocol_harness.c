#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { unsigned unused; } CAN_HandleTypeDef;
typedef struct { uint32_t StdId,IDE,RTR,DLC; } CAN_TxHeaderTypeDef;
CAN_HandleTypeDef hcan1;
#define CAN_ID_STD 0
#define CAN_RTR_DATA 0
#define HAL_OK 0
static uint32_t clock_us,free_slots=3,sent;
static struct {uint32_t id;uint8_t data[8];} frames[16];
static uint32_t __get_PRIMASK(void){return 0;}
static void __disable_irq(void){}
static void __set_PRIMASK(uint32_t m){(void)m;}
static uint32_t HAL_GetUIDw0(void){return 123;}
static uint32_t HAL_GetUIDw1(void){return 456;}
static uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *c){(void)c;return free_slots;}
static int HAL_CAN_AddTxMessage(CAN_HandleTypeDef *c,CAN_TxHeaderTypeDef *h,uint8_t *d,uint32_t *mb){
    (void)c;*mb=0;assert(sent<16);frames[sent].id=h->StdId;
    memcpy(frames[sent++].data,d,8);return HAL_OK;
}
uint32_t VehicleClock_Us32(void){return clock_us;}
uint64_t VehicleClock_NowUs(void){return clock_us;}
#include "board_time_sync.c"
int main(void){
    uint8_t data[8]={0},reply[8]={0};
    clock_us=100000;BoardTimeSync_Init(true);BoardTimeSync_Poll();assert(sent==0);
    BoardTimeSync_OnCan(0x100,data,5,clock_us);BoardTimeSync_Poll();assert(sent==0);
    data[7]=TS_SENSOR_MARKER;
    BoardTimeSync_OnCan(0x100,data,8,clock_us);BoardTimeSync_Poll();assert(sent==1);
    assert(frames[0].id==TS_ID_REQUEST);
    uint32_t key=ts_u32(frames[0].data);
    ts_put32(reply,key+1);ts_put32(reply+4,5000100);
    BoardTimeSync_OnCan(TS_ID_RX_STAMP,reply,8,100200);
    assert(!have_rx);
    ts_put32(reply,key);
    BoardTimeSync_OnCan(TS_ID_RX_STAMP,reply,8,100200);
    ts_put32(reply+4,5000110);
    BoardTimeSync_OnCan(TS_ID_TX_STAMP,reply,8,100300);
    clock_us=100400;BoardTimeSync_Poll();assert(BoardTimeSync_Status(clock_us).count==1);
    clock_us=700000;BoardTimeSync_Poll();assert(sent==1); // peer expired
    BoardTimeSync_Init(false);free_slots=1;
    ts_put32(reply,42);BoardTimeSync_OnCan(TS_ID_REQUEST,reply,8,699950);assert(sent==1);
    free_slots=3;BoardTimeSync_OnCan(TS_ID_REQUEST,reply,8,699950);
    assert(sent==3 && frames[1].id==TS_ID_RX_STAMP && frames[2].id==TS_ID_TX_STAMP);
    assert(ts_u32(frames[1].data)==42 && ts_u32(frames[1].data+4)==699950);
    assert(ts_u32(frames[2].data+4)==700000);
    assert(BoardTimeSync_SendSensor(1234,2345,1,699000,50));
    assert(sent==5 && frames[3].id==0x100 && frames[4].id==TS_ID_SENSOR_TIME);
    assert(ts_u16(frames[3].data)==1234 && ts_u16(frames[3].data+2)==2345);
    assert(ts_u16(frames[3].data+5)==ts_u16(frames[4].data));
    assert(frames[3].data[7]==TS_SENSOR_MARKER && ts_u32(frames[4].data+2)==699000);
    assert(ts_u16(frames[4].data+6)==50);
    free_slots=1;assert(!BoardTimeSync_SendSensor(0,0,0,0,0));assert(sent==5);
    return 0;
}
