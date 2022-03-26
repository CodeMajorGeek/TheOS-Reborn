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

void kprintf(int level, const char* restrict format, ...)
{
    /* TODO: find an algorithm to determine the ideal buffer length. */
    char buff[255];
    size_t len = 255;

    va_list parameters;
    va_start(parameters, format);

    __printf(buff, len, format, parameters);

    va_end(parameters);

#ifdef __USE_QEMU
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_puts(LOGGER_COM_PORT, buff);
#else
    TTY_puts(level_messages[level]);
    TTY_puts(buff);
#endif
}