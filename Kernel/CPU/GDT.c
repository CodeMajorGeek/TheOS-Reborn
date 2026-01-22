#include <CPU/GDT.h>

void GDT_load_TSS_segment(TSS_t* tss)
{
    uintptr_t base = (uintptr_t) tss;
    uint32_t limit = sizeof (TSS_t) - 1;

    TSS_GDT_segment.base_lo = base & 0xFFFF;
    TSS_GDT_segment.base_midl = (base >> 16) & 0xFF;
    TSS_GDT_segment.base_midh = (base >> 24) & 0xFF;
    TSS_GDT_segment.base_hi = base >> 32;

    // TSS64 access byte: 0x89 = Present (bit 7) | TSS64 (bit 0) | Available (bit 4)
    TSS_GDT_segment.access = 0x89;

    TSS_GDT_segment.limit_lo = limit & 0xFFFF;
    // limit_hi_flags: bits 0-3 = limit bits 16-19, bit 4 = 0 (byte granularity), bits 5-6 = 0, bit 7 = 0
    TSS_GDT_segment.limit_hi_flags = (limit >> 16) & 0xF;
    
    // IMPORTANT: reserved field must be 0
    TSS_GDT_segment.rsv = 0;
    
}
