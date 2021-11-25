#ifndef _IRQ_H
#define _IRQ_H

#include <CPU/IDT.h>
#include <CPU/IO.h>

#define MAX_IRQ_ENTRIES 16

#define PIC1_ADDRESS    0x20
#define PIC2_ADDRESS    0xA0
#define PIC_EOI         0x20

IRQ_t irq_handlers[MAX_IRQ_ENTRIES];

void IRQ_handler(interrupt_frame_t frame);

void IRQ_register(int index, IRQ_t irq);

#endif