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
    /* Pushed by PUSH_REGS macro (in reverse order of push) */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;

    /* Pushed by interrupt stub */
    uint64_t int_no;
    uint64_t err_code;

    /* Pushed by CPU (iretq frame) */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    /* Only if CPL change (user -> kernel) */
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

extern void* ISR_stub_table[];
extern void* ISR_stub_table_end[];

__attribute__((aligned(0x10))) static IDT_entry_t IDT[IDT_MAX_DESCRIPTORS];
static IDTR_t idtr;

void IDT_init(void);
void IDT_set_descriptor(uint8_t vector, void* isr, uint8_t flags);

#endif
