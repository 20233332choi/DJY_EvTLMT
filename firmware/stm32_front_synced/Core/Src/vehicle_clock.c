#include "vehicle_clock.h"
#include "main.h"

/* TIM5 is reserved for timekeeping, no pins or control timer changes.
 * Units are nominal microseconds of this board's oscillator (not UTC).
 * Call at least once per 2^32 us; both boards call at 100 Hz. */
static uint64_t upper;
void VehicleClock_Init(void) {
    __HAL_RCC_TIM5_CLK_ENABLE();
    uint32_t hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0u) hz *= 2u;
    TIM5->CR1 = 0;
    TIM5->PSC = hz / 1000000u - 1u;
    TIM5->ARR = 0xffffffffu;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR = 0;
    TIM5->CNT = 0;
    upper = 0;
    TIM5->CR1 = TIM_CR1_CEN;
}
uint64_t VehicleClock_NowUs(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t low = TIM5->CNT;
    if (TIM5->SR & TIM_SR_UIF) {
        TIM5->SR = (uint32_t)~TIM_SR_UIF;
        upper += (1ull << 32);
        low = TIM5->CNT;
    }
    uint64_t value = upper | low;
    __set_PRIMASK(mask);
    return value;
}
uint32_t VehicleClock_Us32(void) { return (uint32_t)VehicleClock_NowUs(); }
