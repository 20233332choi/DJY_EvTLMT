#ifndef TIMING_DIAG_H
#define TIMING_DIAG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
void Timing_Start(void);
void Timing_ImuPacket(uint8_t kind,uint32_t received_us,uint32_t parsed_us);
void Timing_RpmEdge(unsigned side,uint32_t captured_us,bool valid_period);
void Timing_ControlBegin(uint32_t now);
void Timing_ControlEnd(uint32_t started);
void Timing_Publish(void);
int Timing_FormatRelay(char *destination,size_t capacity);
#endif
