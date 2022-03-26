#include <Device/TTY.h>

static uint8_t TTY_row;
static uint8_t TTY_col;
static uint8_t TTY_color;

static uint16_t* TTY_buffer;

static bool TTY_cursor_enabled;

void TTY_init(void)
{
    TTY_set_color(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    TTY_buffer = VGA_BUFFER;
    TTY_cursor_enabled = FALSE;
    TTY_clear();
    TTY_enable_cursor(TRUE);
}

void TTY_clear(void)
{
    TTY_row = 0;
    TTY_col = 0;
    memsetw(TTY_buffer, vga_entry(' ', TTY_color), VGA_WIDTH * VGA_HEIGHT);
    TTY_update_cursor(TTY_col, TTY_row);
}

void TTY_set_color(uint8_t color)
{
    TTY_color = color;
}

void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    const size_t index = y * VGA_WIDTH + x;
    TTY_buffer[index] = vga_entry(c, color);
}

void TTY_putc(char c)
{
#ifdef USE_COM2_OUTPUT
    COM_putc(TTY_COM_PORT, c);
#endif

    if (TTY_row >= VGA_HEIGHT - 1)
    {
        memcpyw(TTY_buffer, TTY_buffer + VGA_WIDTH, VGA_WIDTH * (VGA_HEIGHT - 2) * sizeof (uint16_t));
        --TTY_row;
    }

    switch (c)
    {
        case '\n':
            ++TTY_row;
        case '\r':
            TTY_col = 0;
            return;
        case '\t':
            TTY_col += 4;
            break;
        default:
            TTY_put_entry_at(c, TTY_color, TTY_col, TTY_row);
            break;
    }

    if (++TTY_col >= VGA_WIDTH)
    {
        TTY_col = 0;
        if (TTY_row >= VGA_HEIGHT)
            TTY_row = 0;
    }

    TTY_update_cursor(TTY_col, TTY_row);
}

void TTY_write(const char* str, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        TTY_putc(str[i]);
}

void TTY_puts(const char* str)
{
    TTY_write(str, strlen(str));
}

void TTY_enable_cursor(bool enabled)
{
    if (enabled == TTY_cursor_enabled)
        return;

    /* Set the cursor shape. */
    IO_outb(CURSOR_CTRL, 0x0A);
    uint8_t cursor_shape = IO_inb(CURSOR_DATA) & 0b00011111;

    if (enabled)
        IO_outb(CURSOR_DATA, cursor_shape & ~0x20);
    else
        IO_outb(CURSOR_DATA, cursor_shape | 0x20);

    TTY_cursor_enabled = enabled;
}

void TTY_update_cursor(uint8_t x, uint8_t y)
{
    uint16_t pos = (y + 1) * VGA_WIDTH + x;

    /* 8 lower bits of the cursor position. */
    IO_outb(CURSOR_CTRL, CURSOR_POS + 1);
    IO_outb(CURSOR_DATA, (uint8_t) (pos & 0xFF));

    /* 8 higher bits of the cursor position. */
    IO_outb(CURSOR_CTRL, CURSOR_POS);
    IO_outb(CURSOR_DATA, (uint8_t) ((pos >> 8) & 0xFF));
}