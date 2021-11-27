#include <Device/COM.h>
#include <CPU/IO.h>

#include <string.h>

// Code taken from <https://wiki.osdev.org/Serial_Ports#Example_Code>

bool COM_init(void)
{
    IO_outb(COM_PORT + 1, 0x00); // Disable all interrupts
    IO_outb(COM_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    IO_outb(COM_PORT + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    IO_outb(COM_PORT + 1, 0x00); //                  (hi byte)
    IO_outb(COM_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    IO_outb(COM_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    IO_outb(COM_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
    IO_outb(COM_PORT + 4, 0x1E); // Set in loopback mode, test the serial chip
    IO_outb(COM_PORT + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (IO_inb(COM_PORT + 0) != 0xAE)
    {
        return false;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    IO_outb(COM_PORT + 4, 0x0F);
    return true;
}

bool COM_transmit_empty()
{
    return IO_inb(COM_PORT + 5) & 0x20;
}

void COM_putc(char c)
{
    while(!COM_transmit_empty());

    IO_outb(COM_PORT, c);
}

void COM_write(const char *str, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        COM_putc(str[i]);
}

void COM_puts(const char *str)
{
    COM_write(str, strlen(str));
}