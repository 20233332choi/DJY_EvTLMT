#include "front_timing.h"
#include "main.h"
#include <stdio.h>
extern UART_HandleTypeDef huart2;
#define FRONT_QUEUE 256u
static FrontTimingSample queue[FRONT_QUEUE];
static volatile uint32_t head,tail,dropped;
void FrontTiming_Record(const FrontTimingSample *sample) {
    uint32_t h=head;
    if(h-tail>=FRONT_QUEUE){dropped++;return;}
    queue[h%FRONT_QUEUE]=*sample;__DMB();head=h+1;
}
void FrontTiming_Poll(void) {
    static char line[200];
    if(huart2.gState!=HAL_UART_STATE_READY || head==tail)return;
    __DMB();FrontTimingSample s=queue[tail%FRONT_QUEUE];
    int n=snprintf(line,sizeof(line),
      "F1,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%u,%u,%08lX,%lu\r\n",
      (unsigned long)s.seq,(unsigned long)s.started,(unsigned long)s.period,
      (unsigned long)s.sas_us,(unsigned long)s.tps_us,(unsigned long)s.acquire_us,
      (unsigned long)s.enqueue_us,(unsigned long)s.work_us,
      (unsigned)s.sas,(unsigned)s.tps,(unsigned)s.flags,(unsigned long)s.esr,
      (unsigned long)dropped);
    if(n>0 && n<(int)sizeof(line) &&
       HAL_UART_Transmit_IT(&huart2,(uint8_t *)line,(uint16_t)n)==HAL_OK) {
        __DMB();tail++;
    }
}
