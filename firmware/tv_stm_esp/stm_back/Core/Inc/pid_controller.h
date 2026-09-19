#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H
#include <stdbool.h>

typedef struct {
    float Kp, Ki, Kd;
    float integral;
    float prev_error;
    float integral_max;   /* |적분| 상한 */
    float output_max;     /* |출력| 상한 */
    bool  first_run;
} PID_State;

void  PID_Init(PID_State *p, float Kp, float Ki, float Kd,
               float integral_max, float output_max);
void  PID_Reset(PID_State *p);
float PID_Update(PID_State *p, float error, float dt);

#endif /* PID_CONTROLLER_H */
