#include <CPU/ISR.h>

#include <stdio.h>

IRQ_t IRQ_handlers[MAX_IRQ_ENTRIES];

void ISR_register_IRQ(int index, IRQ_t irq)
{
    int irq_index = index - ISR_COUNT_BEFORE_IRQ;
    if (irq_index < MAX_IRQ_ENTRIES)
        IRQ_handlers[irq_index] = irq;
}

void ISR_exception_handler(interrupt_frame_t frame)
{
    if (frame.int_no < MAX_KNOWN_EXCEPTIONS)
    {
        puts(exception_messages[frame.int_no]);
        puts(" Exception Handled !\n");
    }
    else
    {
        puts("Reserved Exception Handled !\n");
    }

    abort();
}

void IRQ_handler(interrupt_frame_t frame)
{
    /* Must send EOI to the PIC. */
    PIC_send_EOI(frame.err_code); // err_code store the irqindex here.   

    IRQ_t handler = IRQ_handlers[frame.err_code];
    if (handler != 0)
        handler(&frame);
}