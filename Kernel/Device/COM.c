#include <Device/COM.h>

bool COM_init(uint16_t port)
{
    IO_outb(port + COM_INTERRUPT_OFFSET, 0x00);     // Disable all interrupts.
    IO_outb(port + COM_CONTROL_OFFSET, 0x80);       // Enable DLAB (set baud rate divisor).
    IO_outb(port + COM_DATA_OFFSET, 0x03);          // Set divisor to 3 (lo byte) 38400 baud.
    IO_outb(port + COM_INTERRUPT_OFFSET, 0x00);     //                  (hi byte)           .
    IO_outb(port + COM_CONTROL_OFFSET, 0x03);       // 8 bits, no parity, one stop bit.
    IO_outb(port + COM_FIFO_OFFSET, 0xC7);          // Enable FIFO, clear them, with 14-byte threshold.
    IO_outb(port + COM_MODEM_CTRL_OFFSET, 0x0B);    // IRQs enabled, RTS/DSR set.

    /* Test serial chip (send byte 0xAE and check if serial returns same byte) */
    IO_outb(port + COM_MODEM_CTRL_OFFSET, 0x1E);    // Set in loopback mode to test the serial chip.
    IO_outb(port + COM_DATA_OFFSET, TEST_LOOPBACK_BYTE);

    if (COM_read(port) != TEST_LOOPBACK_BYTE)
        return false;

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    IO_outb(port + COM_MODEM_CTRL_OFFSET, 0x0F);

    return true;
}

bool COM_transmit_empty(uint16_t port)
{
    return IO_inb(port + COM_MODEM_STATUS_OFFSET) & COM_STATUS_EMPTY_BIT; // Check the status of the com port by checking if STATUS_EMPTY_BIT is set.
}

void COM_putc(uint16_t port, char c)
{
    while(!COM_transmit_empty(port));

    IO_outb(port, c);
}

void COM_write(uint16_t port, const char* str, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        COM_putc(port, str[i]);
}

void COM_puts(uint16_t port, const char* str)
{
    COM_write(port, str, strlen(str));
}

uint8_t COM_read(uint16_t port)
{
    return IO_inb(port + COM_DATA_OFFSET);
}