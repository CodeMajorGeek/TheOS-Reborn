#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>

typedef struct TSS
{
    uint32_t rsv0;
    uint32_t RSP0_lo;
    uint32_t RSP0_hi;
    uint32_t RSP1_lo;
    uint32_t RSP1_hi;
    uint32_t RSP2_lo;
    uint32_t RSP2_hi;
    uint32_t rsv1;
    uint32_t rsv2;
    uint32_t IST1_lo;
    uint32_t IST1_hi;
    uint32_t IST2_lo;
    uint32_t IST2_hi;
    uint32_t IST3_lo;
    uint32_t IST3_hi;
    uint32_t IST4_lo;
    uint32_t IST4_hi;
    uint32_t IST5_lo;
    uint32_t IST5_hi;
    uint32_t IST6_lo;
    uint32_t IST6_hi;
    uint32_t IST7_lo;
    uint32_t IST7_hi;
    uint32_t rsv3;
    uint32_t rsv4;
    
    uint16_t rsv5;
    uint16_t IOPB;
} TSS_t;

#endif