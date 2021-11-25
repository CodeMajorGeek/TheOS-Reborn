#include <CPU/IRQ.h>

void IRQ_handler(interrupt_frame_t frame)
{
    /* Must send EOI to the PICs. */
    if (frame.int_no >= 40)
        IO_outb(PIC2_ADDRESS, PIC_EOI); // The slave.
    IO_outb(PIC1_ADDRESS, PIC_EOI);     // The master.

    IRQ_t handler = irq_handlers[frame.int_no];
    if (handler != 0)
        handler(&frame);
}

void IRQ_register(int index, IRQ_t irq)
{
    if (index < MAX_IRQ_ENTRIES)
        irq_handlers[index] = irq;
}