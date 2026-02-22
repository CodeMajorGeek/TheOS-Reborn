#include <Debug/Logger.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef THEOS_KDEBUG_LOG_SERIAL
#define THEOS_KDEBUG_LOG_SERIAL 1
#endif

#ifdef __USE_QEMU
#include <Device/COM.h>
#else
#include <Device/TTY.h>
#endif

#ifdef __USE_QEMU
void logger_init(void)
{
#if THEOS_KDEBUG_LOG_SERIAL
    if (!COM_init(LOGGER_COM_PORT))
        abort();
#endif
}
#else
void logger_init(void)
{
}
#endif
void kputc(int level, char c)
{
#ifdef __USE_QEMU
#if THEOS_KDEBUG_LOG_SERIAL
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_putc(LOGGER_COM_PORT, c);
    COM_putc(LOGGER_COM_PORT, '\n');
#endif
#else
    TTY_puts(level_messages[level]);
    TTY_putc(c);
    TTY_putc('\n');
#endif
}

void kputs(int level, const char* str)
{
#ifdef __USE_QEMU
#if THEOS_KDEBUG_LOG_SERIAL
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_puts(LOGGER_COM_PORT, str);
    COM_putc(LOGGER_COM_PORT, '\n');
#endif
#else
    TTY_puts(level_messages[level]);
    TTY_puts(str);
    TTY_putc('\n');
#endif
}

void kprintf(int level, const char* restrict format, ...)
{
    /* TODO: find an algorithm to determine the ideal buffer length. */
    size_t len = 255;

    char buf[255];
    memset(buf, '\0', len);
    
    va_list parameters;
    va_start(parameters, format);

    __printf(buf, len, format, parameters);

    va_end(parameters);

#ifdef __USE_QEMU
#if THEOS_KDEBUG_LOG_SERIAL
    COM_puts(LOGGER_COM_PORT, level_messages[level]);
    COM_puts(LOGGER_COM_PORT, buf);
#endif
#else
    TTY_puts(level_messages[level]);
    TTY_puts(buf);
#endif
}
