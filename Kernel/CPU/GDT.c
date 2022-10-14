#include <CPU/GDT.h>

void GDT_load_TSS_segment(TSS_t* tss)
{
    uintptr_t base = (uintptr_t) tss;
    uint32_t limit = sizeof (TSS_t) - 1;

    TSS_GDT_segment.base_lo = base & 0xFFFF;
    TSS_GDT_segment.base_midl = (base >> 16) & 0xFF;
    TSS_GDT_segment.base_midh = (base >> 24) & 0xFF;
    TSS_GDT_segment.base_hi = (base >> 32) & 0xFFFF;

    TSS_GDT_segment.access = 0x89;

    TSS_GDT_segment.limit_lo = limit & 0xFFFF;
    TSS_GDT_segment.limit_hi_flags = ((limit >> 16) & 0xF) | 0;
}