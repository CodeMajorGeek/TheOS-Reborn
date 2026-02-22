#include <Device/TTY.h>

#include <Device/VGA.h>

#include "TTYBackend.h"

static uint32_t TTY_row = 0;
static uint32_t TTY_col = 0;
static uint8_t TTY_color = 0;
static bool TTY_cursor_enabled = false;
static bool TTY_use_framebuffer = false;
static uint32_t TTY_cols = VGA_WIDTH;
static uint32_t TTY_rows = VGA_HEIGHT;

static void TTY_refresh_grid(void)
{
    if (TTY_use_framebuffer && TTYFB_is_ready())
        TTYFB_get_grid(&TTY_cols, &TTY_rows);
    else
    {
        TTY_cols = VGA_WIDTH;
        TTY_rows = VGA_HEIGHT;
    }

    if (TTY_cols == 0)
        TTY_cols = 1;
    if (TTY_rows == 0)
        TTY_rows = 1;
}

static void TTY_scroll_one_line(void)
{
    if (TTY_use_framebuffer)
        TTYFB_scroll_one_line(TTY_color);
    else
        TTYVGA_scroll_one_line(TTY_color);
}

void TTY_set_buffer(uint16_t* buffer)
{
    TTYVGA_set_buffer(buffer);
}

void TTY_init(void)
{
    TTY_row = 0;
    TTY_col = 0;
    TTY_color = vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK);
    TTY_cursor_enabled = false;
    TTY_use_framebuffer = false;

    TTYVGA_init(TTY_color);
    TTY_refresh_grid();
    TTY_clear();
    TTY_enable_cursor(true);
}

bool TTY_init_framebuffer(const TTY_framebuffer_info_t* info)
{
    if (!TTYFB_init(info, TTY_color))
        return false;

    TTY_use_framebuffer = true;
    TTY_row = 0;
    TTY_col = 0;
    TTY_refresh_grid();
    TTY_clear();
    TTY_enable_cursor(true);
    return true;
}

bool TTY_has_framebuffer(void)
{
    return TTY_use_framebuffer && TTYFB_is_ready();
}

bool TTY_load_psf2(const uint8_t* data, size_t size)
{
    bool ok = TTYFB_load_psf2(data, size);
    if (!ok)
        return false;

    TTY_refresh_grid();
    if (TTY_row >= TTY_rows)
        TTY_row = TTY_rows - 1;
    if (TTY_col >= TTY_cols)
        TTY_col = 0;
    TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
    return true;
}

void TTY_clear(void)
{
    TTY_row = 0;
    TTY_col = 0;

    if (TTY_use_framebuffer)
        TTYFB_clear(TTY_color);
    else
        TTYVGA_clear(TTY_color);

    TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
}

void TTY_set_color(uint8_t color)
{
    TTY_color = color;
}

void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    if (TTY_use_framebuffer)
        TTYFB_put_entry_at(c, color, x, y);
    else
        TTYVGA_put_entry_at(c, color, x, y);
}

void TTY_putc(char c)
{
#ifdef USE_COM2_OUTPUT
    COM_putc(TTY_COM_PORT, c);
#endif

    if (TTY_cols == 0 || TTY_rows == 0)
        TTY_refresh_grid();

    switch (c)
    {
        case '\b':
            if (TTY_col > 0)
                TTY_col--;
            else if (TTY_row > 0)
            {
                TTY_row--;
                TTY_col = TTY_cols - 1;
            }
            else
            {
                TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
                return;
            }

            TTY_put_entry_at(' ', TTY_color, TTY_col, TTY_row);
            TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
            return;
        case '\n':
            TTY_col = 0;
            TTY_row++;
            if (TTY_row >= TTY_rows)
            {
                TTY_scroll_one_line();
                TTY_row = TTY_rows - 1;
            }
            TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
            return;
        case '\r':
            TTY_col = 0;
            TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
            return;
        case '\t':
        {
            uint32_t tab_stop = (TTY_col + 4U) & ~3U;
            while (TTY_col < tab_stop)
            {
                TTY_put_entry_at(' ', TTY_color, TTY_col, TTY_row);
                TTY_col++;
                if (TTY_col >= TTY_cols)
                {
                    TTY_col = 0;
                    TTY_row++;
                    if (TTY_row >= TTY_rows)
                    {
                        TTY_scroll_one_line();
                        TTY_row = TTY_rows - 1;
                    }
                }
            }
            TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
            return;
        }
        default:
            TTY_put_entry_at(c, TTY_color, TTY_col, TTY_row);
            break;
    }

    TTY_col++;
    if (TTY_col >= TTY_cols)
    {
        TTY_col = 0;
        TTY_row++;
        if (TTY_row >= TTY_rows)
        {
            TTY_scroll_one_line();
            TTY_row = TTY_rows - 1;
        }
    }

    TTY_update_cursor((uint8_t) TTY_col, (uint8_t) TTY_row);
}

void TTY_write(const char* str, size_t len)
{
    for (size_t i = 0; i < len; i++)
        TTY_putc(str[i]);
}

void TTY_puts(const char* str)
{
    TTY_write(str, strlen(str));
}

void TTY_enable_cursor(bool enabled)
{
    TTY_cursor_enabled = enabled;

    if (TTY_use_framebuffer)
    {
        TTYFB_enable_cursor(enabled);
        TTYVGA_enable_cursor(false);
        return;
    }

    TTYVGA_enable_cursor(enabled);
}

void TTY_update_cursor(uint8_t x, uint8_t y)
{
    if (TTY_use_framebuffer)
    {
        TTYFB_update_cursor(x, y);
        return;
    }

    if (!TTY_cursor_enabled)
        return;

    TTYVGA_update_cursor(x, y);
}

void TTY_on_timer_tick(void)
{
    if (!TTY_use_framebuffer || !TTYFB_is_ready())
        return;

    TTYFB_on_timer_tick();
}
