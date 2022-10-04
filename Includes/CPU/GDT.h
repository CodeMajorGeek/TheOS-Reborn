#ifndef _GDT_H
#define _GDT_H

#include <CPU/TSS.h>

#include <stdint.h>

#define NULL_SEGMENT        0x00
#define KERNEL_CODE_SEGMENT 0x08
#define KERNEL_DATA_SEGMENT 0x10
#define USER_CODE_SEGMENT   0x18
#define USER_DATA_SEGMENT   0x20
#define TSS_SYSTEM_SEGMENT  0x28

#define NULL_SEGMENT_SELECTOR           0
#define KERNEL_CODE_SEGMENT_SELECTOR    1
#define KERNEL_DATA_SEGMENT_SELECTOR    2
#define USER_CODE_SEGMENT_SELECTOR      3
#define USER_DATA_SEGMENT_SELECTOR      4
#define TSS_SYSTEM_SEGMENT_SELECTOR     5

typedef struct system_segment_descriptor
{
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t base_midl;
    uint8_t access;
    uint8_t limit_hi_flags;
    uint8_t base_midh;
    uint32_t base_hi;
    uint32_t rsv;
} __attribute((__packed__)) system_segment_descriptor_t;

extern system_segment_descriptor_t TSS_GDT_segment;

void GDT_load_TSS_segment(TSS_t* tss);

#endif