#include <Device/PIC.h>

void PIC_remap(int offset1, int offset2)
{
    IO_outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);   // Starts the initialization sequence (in cascade mode).
    IO_wait();
    IO_outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    IO_wait();
    IO_outb(PIC1_DATA, offset1);                    // ICW2: Master PIC vector offset.
    IO_wait();
    IO_outb(PIC2_DATA, offset2);                    // ICW2: Slave PIC vector offset.
    IO_wait();
    IO_outb(PIC1_DATA, 0x04);                       // ICW3: Tell the Master PIC that there is a slave PIC at IRQ2 (0b0000100).
    IO_wait();
    IO_outb(PIC2_DATA, 0x02);                       // ICW3: Tell the Slave PIC its cascade identity (0b00000001).
    IO_wait();

    IO_outb(PIC1_DATA, ICW4_8086);
    IO_wait();
    IO_outb(PIC2_DATA, ICW4_8086);
    IO_wait();

    /* Restore the saved masks. */
    IO_outb(PIC1_DATA, 0x00);
    IO_outb(PIC1_DATA, 0x00);
}

void PIC_send_EOI(uint8_t irq)
{
    if (irq > 8)
        IO_outb(PIC2_COMMAND, PIC_EOI);
    IO_outb(PIC1_COMMAND, PIC_EOI);
}

void PIC_disable(void)
{
    IO_outb(PIC2_DATA, 0xFF);
    IO_outb(PIC1_DATA, 0xFF);
}