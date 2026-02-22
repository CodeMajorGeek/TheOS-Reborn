#include "TTYBackend.h"

#include <Device/VGA.h>
#include <CPU/IO.h>

#include <string.h>

static uint16_t* TTYVGA_buffer = (uint16_t*) VGA_BUFFER_ADDRESS;
static bool TTYVGA_cursor_enabled = false;

void TTYVGA_set_buffer(uint16_t* buffer)
{
    if (buffer)
        TTYVGA_buffer = buffer;
}

void TTYVGA_init(uint8_t color)
{
    TTYVGA_cursor_enabled = false;
    TTYVGA_clear(color);
}

void TTYVGA_clear(uint8_t color)
{
    if (!TTYVGA_buffer)
        return;

    memsetw(TTYVGA_buffer, vga_entry(' ', color), VGA_WIDTH * VGA_HEIGHT);
}

void TTYVGA_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    if (!TTYVGA_buffer || x >= VGA_WIDTH || y >= VGA_HEIGHT)
        return;

    TTYVGA_buffer[y * VGA_WIDTH + x] = vga_entry((unsigned char) c, color);
}

void TTYVGA_scroll_one_line(uint8_t color)
{
    if (!TTYVGA_buffer)
        return;

    for (size_t y = 1; y < VGA_HEIGHT; y++)
    {
        for (size_t x = 0; x < VGA_WIDTH; x++)
            TTYVGA_buffer[(y - 1U) * VGA_WIDTH + x] = TTYVGA_buffer[y * VGA_WIDTH + x];
    }

    uint16_t blank = vga_entry(' ', color);
    for (size_t x = 0; x < VGA_WIDTH; x++)
        TTYVGA_buffer[(VGA_HEIGHT - 1U) * VGA_WIDTH + x] = blank;
}

void TTYVGA_enable_cursor(bool enabled)
{
    if (enabled == TTYVGA_cursor_enabled)
        return;

    IO_outb(CURSOR_CTRL, 0x0A);
    uint8_t cursor_shape = IO_inb(CURSOR_DATA) & 0b00011111;
    if (enabled)
        IO_outb(CURSOR_DATA, cursor_shape & ~0x20);
    else
        IO_outb(CURSOR_DATA, cursor_shape | 0x20);

    TTYVGA_cursor_enabled = enabled;
}

void TTYVGA_update_cursor(uint8_t x, uint8_t y)
{
    if (!TTYVGA_cursor_enabled)
        return;

    uint16_t pos = (uint16_t) (y * VGA_WIDTH + x);

    IO_outb(CURSOR_CTRL, CURSOR_POS + 1);
    IO_outb(CURSOR_DATA, (uint8_t) (pos & 0xFF));
    IO_outb(CURSOR_CTRL, CURSOR_POS);
    IO_outb(CURSOR_DATA, (uint8_t) ((pos >> 8) & 0xFF));
}
