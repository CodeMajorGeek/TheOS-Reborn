#include <CPU/IDT.h>

#include <Device/PIC.h>
#include <CPU/GDT.h>

#include <stdbool.h>

void IDT_init(void)
{
    idtr.base = (uint64_t) &IDT[0];
    idtr.limit = (uint16_t) sizeof(IDT_entry_t) * IDT_MAX_DESCRIPTORS - 1;

    PIC_remap(IRQ_BASE, IRQ_BASE + 8);

    for (uint8_t vector = 0; vector < IDT_MAX_VECTORS; ++vector)
        IDT_set_descriptor(vector, ISR_stub_table[vector], 0x8E);

    __asm__ __volatile__("lidt %0" : : "m"(idtr));  // Load the new IDT.
    __asm__ __volatile__("sti");                    // Set the interrupt FLAG.
}

void IDT_set_descriptor(uint8_t vector, void* isr, uint8_t flags)
{
    IDT_entry_t* desc = &IDT[vector];

    desc->base_low      = (uint64_t) isr & 0xFFFF;
    desc->kernel_cs     = KERNEL_CODE_SEGMENT;
    desc->ist           = 0;
    desc->attributes    = flags;
    desc->base_mid      = ((uint64_t) isr >> 16) & 0xFFFF;
    desc->base_high     = ((uint64_t) isr >> 32) & 0xFFFFFFFF;
    desc->reserved      = 0;
}