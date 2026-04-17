#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <syscall.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/runtime/interrupt_char.h"

static bool g_ctrl_down = false;
static bool g_ext_prefix = false;

mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t) sys_tick_get();
}

void theos_mphal_poll_interrupt(void)
{
    if (mp_interrupt_char < 0)
        return;

    for (;;)
    {
        int code = sys_kbd_get_scancode();
        if (code <= 0)
            return;

        uint8_t sc = (uint8_t) code;
        if (sc == 0xE0U)
        {
            g_ext_prefix = true;
            continue;
        }

        if (g_ext_prefix)
        {
            g_ext_prefix = false;
            if (sc == 0x1DU)
            {
                g_ctrl_down = true;
                continue;
            }
            if (sc == 0x9DU)
            {
                g_ctrl_down = false;
                continue;
            }
        }

        if (sc == 0x1DU)
        {
            g_ctrl_down = true;
            continue;
        }
        if (sc == 0x9DU)
        {
            g_ctrl_down = false;
            continue;
        }
        if ((sc & 0x80U) != 0)
            continue;

        // Set-1 scancode for the 'C' key.
        if (g_ctrl_down && sc == 0x2EU && mp_interrupt_char == 3)
            mp_sched_keyboard_interrupt();
    }
}

int mp_hal_stdin_rx_chr(void)
{
    for (;;)
    {
        int c = getchar();
        if (c == EOF)
        {
            sys_yield();
            continue;
        }

        return c;
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    if (!str || len == 0)
        return 0;

    if (sys_console_write(str, len) < 0)
        return 0;

    return (mp_uint_t) len;
}
