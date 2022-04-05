#ifndef _IDT_H
#define _IDT_H

#include <stdint.h>

#define IDT_MAX_DESCRIPTORS 256

#define IDT_MAX_VECTORS     IDT_MAX_DESCRIPTORS

#define IRQ_BASE            0x20

typedef struct IDT_entry
{
    uint16_t    base_low;   // The lower 0 to 15 bits of the ISR's address.
    uint16_t    kernel_cs;  // The GDT segment loaded by the CPU into CS before calling the ISR.
    uint8_t     ist;        // The IST int the TSS loaded by the CPU into RSP before calling the ISR.
    uint8_t     attributes; // Type and attributes.
    uint16_t    base_mid;   // The middle 16 to 31 bits of the ISR's address.
    uint32_t    base_high;  // The higer 32 to 63 bits of the ISR's address.
    uint32_t    reserved;   // Reserved, set to zero.
} __attribute__((packed)) IDT_entry_t;

typedef struct IDTR
{
    uint16_t    limit;
    uint64_t    base;
} __attribute__((packed)) IDTR_t;

typedef struct interrupt_frame
{
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, userrsp, ss;
} interrupt_frame_t;

extern void* ISR_stub_table[];

__attribute__((aligned(0x10))) static IDT_entry_t IDT[IDT_MAX_DESCRIPTORS];
static IDTR_t idtr;

void IDT_init(void);
void IDT_set_descriptor(uint8_t vector, void* isr, uint8_t flags);

#endif