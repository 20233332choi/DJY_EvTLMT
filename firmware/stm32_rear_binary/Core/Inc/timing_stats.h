#ifndef TIMING_STATS_H
#define TIMING_STATS_H
#include <stdint.h>
#include <limits.h>
/* Cumulative histograms cover EVERY control cycle, not only the 5 Hz UI samples. */
static const uint32_t timing_edges[]={100,250,500,1000,2000,5000,10000,20000,50000,100000,UINT32_MAX};
#define TIMING_BINS 11
typedef struct {uint32_t n,min,max,bins[TIMING_BINS];uint64_t sum;} TimingStat;
static inline void TimingStat_Add(TimingStat *s,uint32_t v) {
    if(!s->n || v<s->min)s->min=v;
    if(v>s->max)s->max=v;
    s->n++;s->sum+=v;
    for(unsigned i=0;i<TIMING_BINS;i++)if(v<=timing_edges[i]){s->bins[i]++;break;}
}
static inline uint32_t TimingStat_P95(const TimingStat *s) {
    uint64_t target=((uint64_t)s->n*95+99)/100;uint32_t count=0;
    if(!s->n)return UINT32_MAX;
    for(unsigned i=0;i<TIMING_BINS;i++){count+=s->bins[i];if(count>=target)return timing_edges[i];}
    return UINT32_MAX;
}
#endif
