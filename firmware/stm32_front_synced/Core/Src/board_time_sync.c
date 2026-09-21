#include "board_time_sync.h"
#include "vehicle_clock.h"
#include "main.h"
extern CAN_HandleTypeDef hcan1;
static bool master;
static TS_Model model;
static uint32_t next_request, cookie;
static volatile uint32_t active_cookie,t1,t2,t3,t4;
static volatile bool awaiting,have_rx,completed;
static uint16_t sensor_seq;
static volatile bool peer_seen;
static volatile uint32_t peer_last;

static bool send(uint32_t id,const uint8_t *d) {
    CAN_TxHeaderTypeDef h={0}; uint32_t mailbox;
    h.StdId=id;h.IDE=CAN_ID_STD;h.RTR=CAN_RTR_DATA;h.DLC=8;
    return HAL_CAN_AddTxMessage(&hcan1,&h,(uint8_t *)d,&mailbox)==HAL_OK;
}
void BoardTimeSync_Init(bool is_master) {
    master=is_master;TS_Reset(&model);next_request=VehicleClock_Us32();
    // Request cookie changes with boot timing; all matching also has a 20 ms deadline.
    cookie=HAL_GetUIDw0()^HAL_GetUIDw1()^VehicleClock_Us32();
    awaiting=have_rx=completed=false;
    peer_seen=false;peer_last=0;
}
bool BoardTimeSync_OnCan(uint32_t id,const uint8_t *d,uint8_t len,uint32_t received) {
    // Front streams stamped sensor data before clock lock. No probes on an
    // isolated Rear or a legacy-only bus: there may be nobody to acknowledge TX.
    if(master && id==0x100u && len==8u && d[7]==TS_SENSOR_MARKER) {
        peer_last=received;peer_seen=true;
    }
    if(id<TS_ID_REQUEST || id>TS_ID_TX_STAMP)return false;
    if(len!=8)return true;
    uint32_t key=ts_u32(d);
    if(!master && id==TS_ID_REQUEST) {
        // ISR-to-ISR stamps include bounded software latency. Do not wait for mailboxes.
        uint32_t mask=__get_PRIMASK();__disable_irq();
        if(HAL_CAN_GetTxMailboxesFreeLevel(&hcan1)>=2u) {
            uint8_t reply[8];ts_put32(reply,key);ts_put32(reply+4,received);
            if(send(TS_ID_RX_STAMP,reply)) {
                ts_put32(reply+4,VehicleClock_Us32());send(TS_ID_TX_STAMP,reply);
            }
        }
        __set_PRIMASK(mask);
    } else if(master && awaiting && key==active_cookie && received-t1<=20000u) {
        if(id==TS_ID_RX_STAMP){t2=ts_u32(d+4);have_rx=true;}
        if(id==TS_ID_TX_STAMP && have_rx){t3=ts_u32(d+4);t4=received;completed=true;awaiting=false;}
    }
    return true;
}
void BoardTimeSync_Poll(void) {
    (void)VehicleClock_NowUs();
    if(!master)return;
    uint32_t a=0,b=0,c=0,d=0;bool done;
    uint32_t mask=__get_PRIMASK();__disable_irq();
    done=completed;if(done){a=t1;b=t2;c=t3;d=t4;completed=false;}
    __set_PRIMASK(mask);
    if(done) {
        // Regression runs outside the control ISR. Publish a coherent model.
        TS_Model next=model;TS_Add(&next,a,b,c,d);
        mask=__get_PRIMASK();__disable_irq();model=next;__set_PRIMASK(mask);
    }
    uint32_t now=VehicleClock_Us32();
    if(!peer_seen || now-peer_last>500000u)return;
    if((int32_t)(now-next_request)<0)return;
    next_request=now+100000u;
    mask=__get_PRIMASK();__disable_irq();
    awaiting=false;have_rx=false;
    if(HAL_CAN_GetTxMailboxesFreeLevel(&hcan1)>0) {
        uint8_t request[8];active_cookie=++cookie;t1=VehicleClock_Us32();
        ts_put32(request,active_cookie);ts_put32(request+4,t1);
        awaiting=send(TS_ID_REQUEST,request);
    }
    __set_PRIMASK(mask);
}
bool BoardTimeSync_Map(uint32_t front,uint32_t now,uint32_t *rear,uint32_t *bound) {
    // Readers are ISR or main; publication masks interrupts.
    return TS_Map(&model,front,now,rear,bound);
}
BoardTimeSyncStatus BoardTimeSync_Status(uint32_t now) {
    BoardTimeSyncStatus s={0};uint32_t unused,bound=0;
    s.valid=TS_Map(&model,model.remote_ref,now,&unused,&bound);
    s.count=model.count;s.rtt_us=model.rtt;s.bound_us=bound;
    s.age_us=model.count?now-model.updated:UINT32_MAX;
    s.drift_ppm=(int32_t)lroundf((model.rate-1)*1000000);
    s.rejected=model.rejected;return s;
}
bool BoardTimeSync_SendSensor(uint16_t sas,uint16_t tps,uint8_t flags,uint32_t sampled,uint16_t span) {
    uint32_t mask=__get_PRIMASK();__disable_irq();
    bool ok=false;
    if(HAL_CAN_GetTxMailboxesFreeLevel(&hcan1)>=2u) {
        uint8_t data[8],stamp[8];uint16_t seq=++sensor_seq;
        ts_put16(data,sas&0x3fff);ts_put16(data+2,tps&0xfff);data[4]=flags;
        ts_put16(data+5,seq);data[7]=TS_SENSOR_MARKER;
        ts_put16(stamp,seq);ts_put32(stamp+2,sampled);ts_put16(stamp+6,span);
        // Lower-ID data is sent first; Rear also accepts opposite arrival order.
        ok=send(0x100u,data);if(ok)ok=send(TS_ID_SENSOR_TIME,stamp);
    }
    __set_PRIMASK(mask);return ok;
}
