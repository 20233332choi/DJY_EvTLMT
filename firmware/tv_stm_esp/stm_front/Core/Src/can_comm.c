#include "can_comm.h"
#include "can_messages.h"
#include "board_config.h"
#include "main.h"

/* CubeMX: CAN1 RX=PA11 / TX=PA12 (board_config.h), 500kbps */
extern CAN_HandleTypeDef hcan1;

static CAN_TxHeaderTypeDef tx_sensor, tx_hb;
static uint32_t tx_mailbox;

void CAN_Init(void) {
    /* Board A는 송신 전용이지만, 다른 노드의 ACK가 필요하므로
     * 필터는 형식상 전수락(수신 미사용). */
    CAN_FilterTypeDef f = {0};
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = 0; f.FilterIdLow      = 0;
    f.FilterMaskIdHigh     = 0; f.FilterMaskIdLow  = 0;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    HAL_CAN_ConfigFilter(&hcan1, &f);

    tx_sensor.StdId = CAN_ID_SENSOR_DATA;
    tx_sensor.IDE   = CAN_ID_STD;
    tx_sensor.RTR   = CAN_RTR_DATA;
    tx_sensor.DLC   = CAN_DLC_SENSOR_DATA;
    tx_sensor.TransmitGlobalTime = DISABLE;

    tx_hb       = tx_sensor;
    tx_hb.StdId = CAN_ID_HEARTBEAT;
    tx_hb.DLC   = CAN_DLC_HEARTBEAT;

    HAL_CAN_Start(&hcan1);
}

void CAN_SendSensorData(uint16_t sas_angle, uint16_t tps_raw, uint8_t flags) {
    uint8_t d[CAN_DLC_SENSOR_DATA];
    can_pack_u16(&d[0], sas_angle & 0x3FFFu);  /* 14-bit */
    can_pack_u16(&d[2], tps_raw   & 0x0FFFu);  /* 12-bit */
    d[4] = flags;                              /* SENSOR_FLAG_* (TV 스위치 등) */
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) > 0) {
        HAL_CAN_AddTxMessage(&hcan1, &tx_sensor, d, &tx_mailbox);
    }
}

void CAN_SendHeartbeat(uint8_t status) {
    uint8_t d[1] = { status };
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) > 0) {
        HAL_CAN_AddTxMessage(&hcan1, &tx_hb, d, &tx_mailbox);
    }
}
