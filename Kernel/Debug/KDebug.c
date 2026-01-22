#include <Debug/KDebug.h>

#include <Device/COM.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

static bool kdebug_initialized = false;

void kdebug_init(void)
{
    if (COM_init(KDEBUG_COM_PORT))
    {
        kdebug_initialized = true;
        kdebug_puts("[KDEBUG] Serial debug initialized\n");
    }
}

void kdebug_putc(char c)
{
    if (!kdebug_initialized)
        return;
    
    COM_putc(KDEBUG_COM_PORT, c);
}

void kdebug_puts(const char* str)
{
    if (!kdebug_initialized)
        return;
    
    COM_puts(KDEBUG_COM_PORT, str);
}

static void kdebug_itoa(uint64_t value, char* buf, size_t length, unsigned int base, bool uppercase)
{
    if (base < 2 || base > 36)
        return;

    uint64_t v = value;
    char digits[length];
    size_t index = 0;
    
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        uint64_t digit = v % base;
        if (digit <= 9)
            digits[index++] = '0' + digit;
        else
            digits[index++] = (uppercase ? 'A' : 'a') + (digit - 10);
        v /= base;
    }

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i];

    buf[index] = '\0';
}

void kdebug_hex(uint64_t value, int width)
{
    char buf[17];
    kdebug_itoa(value, buf, sizeof(buf), 16, true);
    
    int len = strlen(buf);
    if (width > len)
    {
        for (int i = 0; i < width - len; i++)
            kdebug_putc('0');
    }
    kdebug_puts(buf);
}

void kdebug_dec(uint64_t value)
{
    char buf[21];
    kdebug_itoa(value, buf, sizeof(buf), 10, false);
    kdebug_puts(buf);
}

void kdebug_printf(const char* format, ...)
{
    if (!kdebug_initialized)
        return;
    
    va_list parameters;
    va_start(parameters, format);

    while (*format != '\0')
    {
        if (*format == '%')
        {
            format++;
            
            if (*format == 's')
            {
                format++;
                const char* str = va_arg(parameters, const char*);
                kdebug_puts(str);
            }
            else if (*format == 'c')
            {
                format++;
                char c = (char) va_arg(parameters, int);
                kdebug_putc(c);
            }
            else if (*format == 'd' || *format == 'i')
            {
                format++;
                int v = va_arg(parameters, int);
                kdebug_dec(v);
            }
            else if (*format == 'u')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_dec(v);
            }
            else if (*format == 'x')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex(v, 0);
            }
            else if (*format == 'X')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex(v, 0);
            }
            else if (*format == 'l')
            {
                format++;
                if (*format == 'l')
                {
                    format++;
                    if (*format == 'X')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_hex(v, 0);
                    }
                    else if (*format == 'x')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_hex(v, 0);
                    }
                    else if (*format == 'u')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_dec(v);
                    }
                    else if (*format == 'd' || *format == 'i')
                    {
                        format++;
                        long long v = va_arg(parameters, long long);
                        if (v < 0)
                        {
                            kdebug_putc('-');
                            v = -v;
                        }
                        kdebug_dec(v);
                    }
                }
            }
            else if (*format == 'p')
            {
                format++;
                void* ptr = va_arg(parameters, void*);
                kdebug_puts("0x");
                kdebug_hex((uintptr_t) ptr, 16);
            }
            else if (*format == '%')
            {
                kdebug_putc('%');
                format++;
            }
            else
            {
                kdebug_putc('%');
                kdebug_putc(*format);
                format++;
            }
        }
        else
        {
            kdebug_putc(*format);
            format++;
        }
    }

    va_end(parameters);
}

