#include <Device/TTY.h>

#include <Device/VGA.h>

#include "TTYBackend.h"

static TTY_runtime_state_t TTY_state = {
    .output_enabled = true,
    .cols = VGA_WIDTH,
    .rows = VGA_HEIGHT,
    .ansi_state = TTY_ANSI_IDLE
};

static void TTY_ansi_reset(void)
{
    TTY_state.ansi_state = TTY_ANSI_IDLE;
    TTY_state.ansi_param = 0;
    TTY_state.ansi_has_param = false;
}

static void TTY_move_cursor_left(uint32_t count)
{
    while (count > 0)
    {
        if (TTY_state.col > 0)
            TTY_state.col--;
        else if (TTY_state.row > 0)
        {
            TTY_state.row--;
            TTY_state.col = TTY_state.cols - 1;
        }
        else
            break;

        count--;
    }

    TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
}

static void TTY_erase_line_from_cursor(void)
{
    uint32_t saved_col = TTY_state.col;
    uint32_t saved_row = TTY_state.row;

    for (uint32_t x = saved_col; x < TTY_state.cols; x++)
        TTY_put_entry_at(' ', TTY_state.color, x, saved_row);

    TTY_update_cursor((uint8_t) saved_col, (uint8_t) saved_row);
}

static void TTY_move_cursor_home(void)
{
    TTY_state.row = 0;
    TTY_state.col = 0;
    TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
}

static bool TTY_try_handle_ansi(char c)
{
    uint8_t uc = (uint8_t) c;

    if (TTY_state.ansi_state == TTY_ANSI_IDLE)
    {
        if (uc == 0x1BU)
        {
            TTY_state.ansi_state = TTY_ANSI_ESC;
            return true;
        }
        return false;
    }

    if (TTY_state.ansi_state == TTY_ANSI_ESC)
    {
        if (c == '[')
        {
            TTY_state.ansi_state = TTY_ANSI_CSI;
            TTY_state.ansi_param = 0;
            TTY_state.ansi_has_param = false;
            return true;
        }

        TTY_ansi_reset();
        return true;
    }

    if (uc >= '0' && uc <= '9')
    {
        TTY_state.ansi_has_param = true;
        uint32_t digit = (uint32_t) (uc - '0');
        if (TTY_state.ansi_param <= 10000U)
            TTY_state.ansi_param = (TTY_state.ansi_param * 10U) + digit;
        return true;
    }

    if (c == ';')
        return true;

    uint32_t param = TTY_state.ansi_has_param ? TTY_state.ansi_param : 1U;
    if (c == 'D')
        TTY_move_cursor_left(param);
    else if (c == 'H')
        TTY_move_cursor_home();
    else if (c == 'J')
    {
        if (param == 2U)
            TTY_clear();
    }
    else if (c == 'K')
        TTY_erase_line_from_cursor();

    TTY_ansi_reset();
    return true;
}

static void TTY_refresh_grid(void)
{
    if (TTY_state.use_framebuffer && TTYFB_is_ready())
        TTYFB_get_grid(&TTY_state.cols, &TTY_state.rows);
    else
    {
        TTY_state.cols = VGA_WIDTH;
        TTY_state.rows = VGA_HEIGHT;
    }

    if (TTY_state.cols == 0)
        TTY_state.cols = 1;
    if (TTY_state.rows == 0)
        TTY_state.rows = 1;
}

static void TTY_scroll_one_line(void)
{
    if (TTY_state.use_framebuffer)
        TTYFB_scroll_one_line(TTY_state.color);
    else
        TTYVGA_scroll_one_line(TTY_state.color);
}

void TTY_set_buffer(uint16_t* buffer)
{
    TTYVGA_set_buffer(buffer);
}

void TTY_init(void)
{
    TTY_state.row = 0;
    TTY_state.col = 0;
    TTY_state.color = vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK);
    TTY_state.cursor_enabled = false;
    TTY_state.use_framebuffer = false;
    TTY_state.output_enabled = true;
    TTY_ansi_reset();

    TTYVGA_init(TTY_state.color);
    TTY_refresh_grid();
    TTY_clear();
    TTY_enable_cursor(true);
}

bool TTY_init_framebuffer(const TTY_framebuffer_info_t* info)
{
    if (!TTYFB_init(info, TTY_state.color))
        return false;

    TTY_state.use_framebuffer = true;
    TTY_state.row = 0;
    TTY_state.col = 0;
    TTY_ansi_reset();
    TTY_refresh_grid();
    TTY_clear();
    TTY_enable_cursor(true);
    return true;
}

bool TTY_has_framebuffer(void)
{
    return TTY_state.use_framebuffer && TTYFB_is_ready();
}

bool TTY_load_psf2(const uint8_t* data, size_t size)
{
    bool ok = TTYFB_load_psf2(data, size);
    if (!ok)
        return false;

    TTY_refresh_grid();
    if (TTY_state.row >= TTY_state.rows)
        TTY_state.row = TTY_state.rows - 1;
    if (TTY_state.col >= TTY_state.cols)
        TTY_state.col = 0;
    TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
    return true;
}

void TTY_clear(void)
{
    TTY_state.row = 0;
    TTY_state.col = 0;
    TTY_ansi_reset();

    if (TTY_state.use_framebuffer)
        TTYFB_clear(TTY_state.color);
    else
        TTYVGA_clear(TTY_state.color);

    TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
}

void TTY_set_color(uint8_t color)
{
    TTY_state.color = color;
}

void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    if (TTY_state.use_framebuffer)
        TTYFB_put_entry_at(c, color, x, y);
    else
        TTYVGA_put_entry_at(c, color, x, y);
}

void TTY_putc(char c)
{
    if (!TTY_state.output_enabled)
        return;

#ifdef USE_COM2_OUTPUT
    COM_putc(TTY_COM_PORT, c);
#endif

    if (TTY_state.cols == 0 || TTY_state.rows == 0)
        TTY_refresh_grid();

    if (TTY_try_handle_ansi(c))
        return;

    switch (c)
    {
        case '\b':
            if (TTY_state.col > 0)
                TTY_state.col--;
            else if (TTY_state.row > 0)
            {
                TTY_state.row--;
                TTY_state.col = TTY_state.cols - 1;
            }
            else
            {
                TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
                return;
            }

            TTY_put_entry_at(' ', TTY_state.color, TTY_state.col, TTY_state.row);
            TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
            return;
        case '\n':
            TTY_state.col = 0;
            TTY_state.row++;
            if (TTY_state.row >= TTY_state.rows)
            {
                TTY_scroll_one_line();
                TTY_state.row = TTY_state.rows - 1;
            }
            TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
            return;
        case '\r':
            TTY_state.col = 0;
            TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
            return;
        case '\t':
        {
            uint32_t tab_stop = (TTY_state.col + 4U) & ~3U;
            while (TTY_state.col < tab_stop)
            {
                TTY_put_entry_at(' ', TTY_state.color, TTY_state.col, TTY_state.row);
                TTY_state.col++;
                if (TTY_state.col >= TTY_state.cols)
                {
                    TTY_state.col = 0;
                    TTY_state.row++;
                    if (TTY_state.row >= TTY_state.rows)
                    {
                        TTY_scroll_one_line();
                        TTY_state.row = TTY_state.rows - 1;
                    }
                }
            }
            TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
            return;
        }
        default:
            TTY_put_entry_at(c, TTY_state.color, TTY_state.col, TTY_state.row);
            break;
    }

    TTY_state.col++;
    if (TTY_state.col >= TTY_state.cols)
    {
        TTY_state.col = 0;
        TTY_state.row++;
        if (TTY_state.row >= TTY_state.rows)
        {
            TTY_scroll_one_line();
            TTY_state.row = TTY_state.rows - 1;
        }
    }

    TTY_update_cursor((uint8_t) TTY_state.col, (uint8_t) TTY_state.row);
}

void TTY_set_output_enabled(bool enabled)
{
    TTY_state.output_enabled = enabled;
    if (!TTY_state.output_enabled)
        TTY_enable_cursor(false);
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
    TTY_state.cursor_enabled = enabled;

    if (TTY_state.use_framebuffer)
    {
        TTYFB_enable_cursor(enabled);
        TTYVGA_enable_cursor(false);
        return;
    }

    TTYVGA_enable_cursor(enabled);
}

void TTY_update_cursor(uint8_t x, uint8_t y)
{
    if (TTY_state.use_framebuffer)
    {
        TTYFB_update_cursor(x, y);
        return;
    }

    if (!TTY_state.cursor_enabled)
        return;

    TTYVGA_update_cursor(x, y);
}

void TTY_on_timer_tick(void)
{
    if (!TTY_state.use_framebuffer || !TTYFB_is_ready())
        return;

    TTYFB_on_timer_tick();
}
