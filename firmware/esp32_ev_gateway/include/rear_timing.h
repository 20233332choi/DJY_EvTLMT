#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// ts= has 19 integer fields. Only drift (11) is signed; all others are uint32.
// Keep this compact for the existing telemetry queue; legacy ts= remains readable.
class RearTiming {
public:
    bool valid=false;
    int64_t fields[19]={};
    bool parse(const char *line) {
        valid=false;
        const char *p=strstr(line," ts=");if(!p)return false;p+=4;
        int64_t next[19];
        for(unsigned i=0;i<19;i++) {
            if((*p<'0'||*p>'9') && !(i==11 && *p=='-'))return false;
            errno=0;char *end;
            long long value=strtoll(p,&end,10);
            if(errno==ERANGE || end==p || value<(i==11?INT32_MIN:0LL) ||
               value>(i==11?INT32_MAX:4294967295LL))return false;
            next[i]=value;p=end;
            if(i<18){if(*p!='/')return false;p++;}
        }
        if(*p!='\0' && *p!=' ' && *p!='\r' && *p!='\n')return false;
        if(next[0]!=1 || next[2]>=13 || next[8]>1 || next[12]>1 || next[13]>65535)return false;
        memcpy(fields,next,sizeof(fields));valid=true;return true;
    }
    bool json(char *out,size_t size) const {
        if(!valid){if(size<5)return false;memcpy(out,"null",5);return true;}
        size_t used=0;
        for(unsigned i=0;i<19;i++) {
            // Values are bounded uint32 or int32, avoid %lld on embedded libc.
            int n=i==11?snprintf(out+used,size-used,"%s%ld",i?",":"[",(long)fields[i]):
                        snprintf(out+used,size-used,"%s%lu",i?",":"[",(unsigned long)fields[i]);
            if(n<0 || (size_t)n>=size-used)return false;
            used+=(size_t)n;
        }
        if(size-used<2)return false;
        out[used++]=']';out[used]='\0';return true;
    }
};
