#ifndef _ISR_H
#define _ISR_H

#include <CPU/IDT.h>
#include <Device/PIC.h>

#include <stdint.h>
#include <stdlib.h>

#define MAX_KNOWN_EXCEPTIONS    19

#define ISR_COUNT_BEFORE_IRQ    32      
#define MAX_IRQ_ENTRIES         16

#define IRQ0    32
#define IRQ1    33
#define IRQ2    34
#define IRQ3    35
#define IRQ4    36
#define IRQ5    37
#define IRQ6    38
#define IRQ7    39
#define IRQ8    40
#define IRQ9    41
#define IRQ10   42
#define IRQ11   43
#define IRQ12   44
#define IRQ13   45
#define IRQ14   46
#define IRQ15   47

typedef void (*IRQ_t)(interrupt_frame_t*);

static const char* exception_messages[MAX_KNOWN_EXCEPTIONS] =
{
"Divide By Zero",
"Debug",
"Not Maskable Interrupt",
"Int 3",
"INTO",
"Out of Bounds",
"Invalid Opcode",
"Coprocessor Not Acailable",
"Double Fault",
"Coprocessor Segment Overrun",
"Bad TSS",
"Stack No Present",
"Stack Fault",
"General Protection Fault",
"Page Fault",
"Reserved",
"Floating Point",
"Alignment Check",
"Machine Check"
};

void ISR_register_IRQ(int index, IRQ_t irq);

void ISR_exception_handler(interrupt_frame_t frame);
void IRQ_handler(interrupt_frame_t frame);

#endif