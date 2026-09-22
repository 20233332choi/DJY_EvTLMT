#include "tps_sensor.h"
#include "board_config.h"
#include "main.h"

/* CubeMX: ADC1_IN0 = PA0 (board_config.h의 TPS_ADC_PIN), 12-bit, 단일변환 */
extern ADC_HandleTypeDef hadc1;
static bool s_io_ok=true;
bool TPS_LastIOOk(void) { return s_io_ok; }

void TPS_Init(void) {
    /* 채널이 board_config.h와 일치하는지 명시적으로 한 번 세팅.
     * (CubeMX에서 이미 설정돼 있어도 무해하며, 핀 변경 시 누락을 막는다.) */
    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel      = TPS_ADC_CHANNEL;
    ch.Rank         = 1;
    ch.SamplingTime = ADC_SAMPLETIME_84CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &ch);

    /* F446은 별도 SW 캘리브레이션 불필요. 첫 변환 안정화용 더미 1회. */
    (void)TPS_ReadRaw();
}

/* ★실차 노이즈 대응 — 오버샘플링 평균.
 * 아날로그 노이즈는 소스에서 줄이는 게 가장 싸고, 무엇보다 "지연이 0"이다.
 * ADC 변환 1회는 (84+12)/21MHz ≈ 4.6us라 8회를 다 돌려도 37us — 10ms 제어
 * 주기에 비하면 없는 시간이다. 반면 백색잡음은 sqrt(8) ≈ 2.8배 줄어든다.
 * 같은 감쇠를 Board B에서 IIR 필터로 얻으려면 수 ms의 위상지연을 감수해야
 * 하므로, 여기서 미리 깎아두는 편이 훨씬 유리하다.
 * 변환이 한 번이라도 실패하면 0을 반환해 TPS_IsValid()가 fault로 잡게 한다. */
#define TPS_OVERSAMPLE  8u

uint16_t TPS_ReadRaw(void) {
    s_io_ok=true;
    uint32_t acc = 0;

    /* ContinuousConvMode=DISABLE 이라 Start 1회당 변환이 정확히 1회만 일어난다.
     * 따라서 샘플마다 Start/Poll/Stop을 한 세트로 돌려야 한다. */
    for (uint8_t i = 0; i < TPS_OVERSAMPLE; i++) {
        if(HAL_ADC_Start(&hadc1)!=HAL_OK)s_io_ok=false;
        if (HAL_ADC_PollForConversion(&hadc1, 2) != HAL_OK) {
            s_io_ok=false;
            HAL_ADC_Stop(&hadc1);
            return 0;   /* 변환 실패 → TPS_IsValid에서 fault 처리 */
        }
        acc += HAL_ADC_GetValue(&hadc1);
        if(HAL_ADC_Stop(&hadc1)!=HAL_OK)s_io_ok=false;
    }

    return (uint16_t)(acc / TPS_OVERSAMPLE);
}

bool TPS_IsValid(uint16_t raw) {
    return (raw >= (TPS_ADC_MIN - TPS_ADC_MARGIN)) &&
           (raw <= (TPS_ADC_MAX + TPS_ADC_MARGIN));
}
