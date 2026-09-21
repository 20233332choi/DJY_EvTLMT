#ifndef TIME_SYNC_MODEL_H
#define TIME_SYNC_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* Pure clock estimator. Signed differences unwrap only bounded (< 2^31 us)
 * exchanges; no wall-clock changes and no subtraction of absolute floats. */
#define TS_POINTS 24u
#define TS_STALE_US 500000u
#define TS_MAX_RTT_US 2000u
#define TS_MAX_BOUND_US 1000u
typedef struct {
    uint32_t remote, rear, rtt;
} TS_Point;
typedef struct {
    TS_Point point[TS_POINTS];
    unsigned count;
    uint32_t remote_ref, rear_ref, updated, rtt, bound, rejected;
    float rate, bias, residual;
    bool ready;
} TS_Model;

static inline void TS_Reset(TS_Model *m) { memset(m, 0, sizeof(*m)); m->rate=1.0f; }
static inline bool TS_Add(TS_Model *m,uint32_t t1,uint32_t t2,uint32_t t3,uint32_t t4) {
    uint32_t outer=t4-t1, service=t3-t2;
    if(outer>20000u || service>10000u) {m->rejected++;return false;}
    float rate=m->ready?m->rate:1.0f;
    float network=(float)outer-rate*(float)service;
    if(network<0 || network>TS_MAX_RTT_US) {m->rejected++;return false;}
    uint32_t remote=t2+service/2u, rear=t1+outer/2u;
    if(m->count) {
        TS_Point last=m->point[m->count-1];
        int32_t dr=(int32_t)(remote-last.remote), dl=(int32_t)(rear-last.rear);
        // Reset/reacquire on reboot, clock step or a gap. Allow wrap and HSI skew.
        if(dr<=0 || dl<=0 || dl>1000000 || fabsf((float)dr-dl)>0.06f*dl+3000) TS_Reset(m);
    }
    if(m->count==TS_POINTS) {memmove(m->point,m->point+1,(TS_POINTS-1)*sizeof(TS_Point));m->count--;}
    m->point[m->count++]=(TS_Point){remote,rear,(uint32_t)ceilf(network)};
    m->remote_ref=remote;m->rear_ref=rear;m->updated=t4;m->rtt=(uint32_t)ceilf(network);
    m->ready=false;
    if(m->count<8 || (uint32_t)(rear-m->point[0].rear)<500000u) return true;
    double sx=0,sy=0,sxx=0,sxy=0;
    for(unsigned i=0;i<m->count;i++) {
        double x=(int32_t)(m->point[i].remote-remote), y=(int32_t)(m->point[i].rear-rear);
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
    }
    double n=m->count, denom=n*sxx-sx*sx;
    if(denom<=0) return true;
    m->rate=(float)((n*sxy-sx*sy)/denom);
    m->bias=(float)((sy-m->rate*sx)/n);
    float residual=0;uint32_t half_rtt=0;
    for(unsigned i=0;i<m->count;i++) {
        float e=fabsf(m->rate*(int32_t)(m->point[i].remote-remote)+m->bias-(int32_t)(m->point[i].rear-rear));
        if(e>residual)residual=e;
        if((m->point[i].rtt+1)/2>half_rtt)half_rtt=(m->point[i].rtt+1)/2;
    }
    m->residual=residual;
    // This is a reported estimate, not a hardware-certified absolute bound.
    m->bound=(uint32_t)ceilf(residual)+half_rtt+50u;
    m->ready=m->rate>=.95f && m->rate<=1.05f && m->bound<=TS_MAX_BOUND_US;
    return true;
}
static inline bool TS_Map(const TS_Model *m,uint32_t remote,uint32_t now,uint32_t *rear,uint32_t *bound) {
    uint32_t age=now-m->updated;
    if(!m->ready || age>TS_STALE_US) return false;
    int32_t delta=(int32_t)(remote-m->remote_ref);
    if(delta < -100000 || delta > 600000) return false;
    // Reserve 500 ppm for extrapolation uncertainty; measured fit error separate.
    *bound=m->bound+(age+1999u)/2000u;
    if(*bound>TS_MAX_BOUND_US)return false;
    *rear=m->rear_ref+(int32_t)lroundf(m->rate*delta+m->bias);
    return true;
}
#endif
