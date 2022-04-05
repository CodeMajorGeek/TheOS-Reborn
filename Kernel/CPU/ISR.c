#include <CPU/ISR.h>

#include <Device/PIC.h>
#include <CPU/Syscall.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

IRQ_t IRQ_handlers[MAX_IRQ_ENTRIES];

void ISR_register_IRQ(int index, IRQ_t irq)
{
    int irq_index = index - ISR_COUNT_BEFORE_IRQ;
    if (irq_index < MAX_IRQ_ENTRIES)
        IRQ_handlers[irq_index] = irq;
}

void ISR_handler(interrupt_frame_t frame)
{
    if (frame.int_no < MAX_KNOWN_EXCEPTIONS)
    {
        puts(exception_messages[frame.int_no]);
        puts(" Exception Handled !\n");
    }
    else if (frame.int_no == SYSCALL_INT)
    {
        Syscall_interupt_handler(&frame);
        return;
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
    PIC_send_EOI(frame.err_code); // err_code store the irq index here.   

    IRQ_t handler = IRQ_handlers[frame.err_code];
    if (handler != 0)
        handler(&frame);
}