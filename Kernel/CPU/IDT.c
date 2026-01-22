#include <CPU/IDT.h>

#include <Device/PIC.h>
#include <CPU/GDT.h>
#include <CPU/Syscall.h>

#include <stdbool.h>

void IDT_init(void)
{
    idtr.base = (uint64_t) &IDT[0];
    idtr.limit = (uint16_t) sizeof(IDT_entry_t) * IDT_MAX_DESCRIPTORS - 1;

    PIC_remap(IRQ_BASE, IRQ_BASE + 8);

    // Default all IDT entries to ring 0 (DPL=0).
    for (int vector = 0; vector < IDT_MAX_VECTORS; ++vector)
        IDT_set_descriptor(vector, ISR_stub_table[vector], 0);

    // Allow only the syscall vector from ring 3.
    IDT_set_descriptor(SYSCALL_INT, ISR_stub_table[SYSCALL_INT], 3);

    __asm__ __volatile__("lidt %0" : : "m"(idtr));  // Load the new IDT.
    __asm__ __volatile__("sti");                    // Set the interrupt FLAG.
}

void IDT_set_descriptor(uint8_t vector, void* isr, uint8_t dpl)
{
    IDT_entry_t* desc = &IDT[vector];

    desc->base_low   = (uint64_t)isr & 0xFFFF;
    desc->base_mid   = ((uint64_t)isr >> 16) & 0xFFFF;
    desc->base_high  = ((uint64_t)isr >> 32) & 0xFFFFFFFF;

    desc->kernel_cs  = KERNEL_CODE_SEGMENT;  // ring0 code segment
    desc->ist        = 0;                     // pas d’IST pour l’instant
    desc->attributes = 0x8E | ((dpl & 0x3) << 5); // interrupt gate + DPL
    desc->reserved   = 0;
}
