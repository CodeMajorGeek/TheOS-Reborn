#include <CPU/ISR.h>

#include <Device/PIC.h>
#include <CPU/Syscall.h>
#include <CPU/APIC.h>
#include <CPU/GDT.h>
#include <Debug/KDebug.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

IRQ_t IRQ_handlers[MAX_IRQ_ENTRIES];

void ISR_register_IRQ(int vector, IRQ_t irq)
{
    int irq_index = vector - ISR_COUNT_BEFORE_IRQ;

    if (irq_index < 0 || irq_index >= MAX_IRQ_ENTRIES)
        return;

    IRQ_handlers[irq_index] = irq;

    if (APIC_is_enabled())
    {
        APIC_register_IRQ_vector(vector, irq_index, FALSE);
    }
}


void ISR_handler(interrupt_frame_t* frame)
{
    if (frame->int_no < MAX_KNOWN_EXCEPTIONS)
    {
        kdebug_printf(
            "[ISR] Exception %llu (%s)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            exception_messages[frame->int_no],
            frame->rip,
            frame->cs,
            frame->rflags
        );
    }
    else
    {
        kdebug_printf(
            "[ISR] Exception %llu (Unknown)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            frame->rip,
            frame->cs,
            frame->rflags
        );
    }

    abort();
}


void IRQ_handler(interrupt_frame_t *frame)
{
    int irq = frame->int_no - ISR_COUNT_BEFORE_IRQ;

    if (irq < 0 || irq >= MAX_IRQ_ENTRIES)
        return;

    IRQ_t handler = IRQ_handlers[irq];
    if (handler)
        handler(frame);

    if (APIC_is_enabled())
        APIC_send_EOI();
    else
        PIC_send_EOI(irq);
}
