#include <Debug/logger.h>

void logger_init(void)
{
#ifdef __USE_QEMU
    if (!COM_init(LOGGER_COM_PORT))
        abort();
#endif
}

void kputc(int level, char c)
{
#ifdef __USE_QEMU
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_putc(LOGGER_COM_PORT, c);
    COM_putc(LOGGER_COM_PORT, '\n');
#else
    TTY_puts(level_messages[level]);
    TTY_putc(c);
    TTY_putc('\n');
#endif
}

void kputs(int level, const char* str)
{
#ifdef __USE_QEMU
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_puts(LOGGER_COM_PORT, str);
    COM_putc(LOGGER_COM_PORT, '\n');
#else
    TTY_puts(level_messages[level]);
    TTY_puts(str);
    TTY_putc('\n');
#endif
}