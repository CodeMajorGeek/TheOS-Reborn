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

void GDT_load_kernel_segments(void)
{
    __asm__ __volatile__("lgdt %0" : : "m"(kernel_gdt64_pointer));

    // Reload CS to the kernel code selector from the freshly loaded GDT.
    // Without this, APs keep the trampoline CS selector value, which may map
    // to a non-code descriptor in the kernel GDT and trigger #GP on iretq.
    __asm__ __volatile__(
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        :
        : "rax", "memory"
    );

    uint16_t data_selector = KERNEL_DATA_SEGMENT;
    __asm__ __volatile__(
        "movw %0, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "rm"(data_selector)
        : "ax", "memory"
    );
}
