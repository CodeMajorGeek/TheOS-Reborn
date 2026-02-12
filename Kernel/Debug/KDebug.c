#include <Debug/KDebug.h>

#ifdef THEOS_ENABLE_KDEBUG
#include <Device/COM.h>
#include <Debug/Spinlock.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

static bool kdebug_initialized = false;
static spinlock_t kdebug_lock = { 0 };

static inline void kdebug_putc_raw(char c)
{
    COM_putc(KDEBUG_COM_PORT, c);
}

static inline void kdebug_puts_raw(const char* str)
{
    COM_puts(KDEBUG_COM_PORT, str);
}

void kdebug_init(void)
{
    if (COM_init(KDEBUG_COM_PORT))
    {
        kdebug_initialized = true;
        spinlock_init(&kdebug_lock);
        kdebug_puts("[KDEBUG] Serial debug initialized\n");
    }
}

void kdebug_putc(char c)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_putc_raw(c);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_puts(const char* str)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_puts_raw(str);
    spin_unlock_irqrestore(&kdebug_lock, flags);
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

static void kdebug_hex_raw(uint64_t value, int width)
{
    char buf[17];
    kdebug_itoa(value, buf, sizeof(buf), 16, true);
    
    int len = strlen(buf);
    if (width > len)
    {
        for (int i = 0; i < width - len; i++)
            kdebug_putc_raw('0');
    }
    kdebug_puts_raw(buf);
}

static void kdebug_dec_raw(uint64_t value)
{
    char buf[21];
    kdebug_itoa(value, buf, sizeof(buf), 10, false);
    kdebug_puts_raw(buf);
}

void kdebug_hex(uint64_t value, int width)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_hex_raw(value, width);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_dec(uint64_t value)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_dec_raw(value);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_printf(const char* format, ...)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
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
                kdebug_puts_raw(str);
            }
            else if (*format == 'c')
            {
                format++;
                char c = (char) va_arg(parameters, int);
                kdebug_putc_raw(c);
            }
            else if (*format == 'd' || *format == 'i')
            {
                format++;
                int v = va_arg(parameters, int);
                kdebug_dec_raw(v);
            }
            else if (*format == 'u')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_dec_raw(v);
            }
            else if (*format == 'x')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex_raw(v, 0);
            }
            else if (*format == 'X')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex_raw(v, 0);
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
                        kdebug_hex_raw(v, 0);
                    }
                    else if (*format == 'x')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_hex_raw(v, 0);
                    }
                    else if (*format == 'u')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_dec_raw(v);
                    }
                    else if (*format == 'd' || *format == 'i')
                    {
                        format++;
                        long long v = va_arg(parameters, long long);
                        if (v < 0)
                        {
                            kdebug_putc_raw('-');
                            v = -v;
                        }
                        kdebug_dec_raw(v);
                    }
                }
            }
            else if (*format == 'p')
            {
                format++;
                void* ptr = va_arg(parameters, void*);
                kdebug_puts_raw("0x");
                kdebug_hex_raw((uintptr_t) ptr, 16);
            }
            else if (*format == '%')
            {
                kdebug_putc_raw('%');
                format++;
            }
            else
            {
                kdebug_putc_raw('%');
                kdebug_putc_raw(*format);
                format++;
            }
        }
        else
        {
            kdebug_putc_raw(*format);
            format++;
        }
    }

    va_end(parameters);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}
#else
void kdebug_init(void)
{
}

void kdebug_putc(char c)
{
    (void)c;
}

void kdebug_puts(const char* str)
{
    (void)str;
}

void kdebug_hex(uint64_t value, int width)
{
    (void)value;
    (void)width;
}

void kdebug_dec(uint64_t value)
{
    (void)value;
}

void kdebug_printf(const char* format, ...)
{
    (void)format;
}
#endif
