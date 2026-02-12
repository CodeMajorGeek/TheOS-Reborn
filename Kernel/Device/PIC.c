#include <Device/PIC.h>

#include <CPU/IO.h>

static uint16_t PIC_get_irq_reg(uint8_t ocw3)
{
    IO_outb(PIC1_COMMAND, ocw3);
    IO_outb(PIC2_COMMAND, ocw3);
    return ((uint16_t) IO_inb(PIC2_COMMAND) << 8) | IO_inb(PIC1_COMMAND);
}

void PIC_remap(int offset1, int offset2)
{
    uint8_t mask1 = IO_inb(PIC1_DATA);
    uint8_t mask2 = IO_inb(PIC2_DATA);

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
    IO_outb(PIC1_DATA, mask1);
    IO_outb(PIC2_DATA, mask2);
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
    IO_wait();
    IO_outb(PIC1_DATA, 0xFF);
}

uint16_t PIC_get_IRR(void)
{
    return PIC_get_irq_reg(PIC_READ_IRR);
}

uint16_t PIC_get_ISR(void)
{
    return PIC_get_irq_reg(PIC_READ_ISR);
}

uint16_t PIC_get_mask(void)
{
    return ((uint16_t) IO_inb(PIC2_DATA) << 8) | IO_inb(PIC1_DATA);
}
