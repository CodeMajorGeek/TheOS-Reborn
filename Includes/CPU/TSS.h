#ifndef _TSS_H
#define _TSS_H

#include <CPU/GDT.h>

#include <stdint.h>

typedef struct TSS
{
    uint32_t r1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t r2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t r3;
    uint16_t r4;
    uint16_t io_mba;
} __attribute__((__packed__))TSS_t;

extern void TSS_flush(uint16_t GDT_selector);

#endif