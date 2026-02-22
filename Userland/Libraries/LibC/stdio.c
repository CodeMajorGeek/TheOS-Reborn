#include <stdio.h>

#if defined(__THEOS_KERNEL)
#include <Device/TTY.h>
#if defined(__THEOS_KERNEL) && defined(THEOS_ENABLE_KDEBUG)
#include <Debug/KDebug.h>
#endif
#else
#include <syscall.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

int putc(int c)
{
#if defined(__THEOS_KERNEL)
    TTY_putc(c);
    return (char) c;
#else
    char ch = (char) c;
    if (sys_console_write(&ch, 1) < 0)
        return EOF;
    return (unsigned char) ch;
#endif
}

int puts(const char *s)
{
#if defined(__THEOS_KERNEL)
    TTY_puts(s);
    return 1;
#else
    if (!s)
        return EOF;

    size_t len = strlen(s);
    if (len == 0)
        return 1;

    return sys_console_write(s, len) < 0 ? EOF : 1;
#endif
}

int __printf(char* buff, size_t buff_len, const char* __restrict format, va_list parameters)
{
    int written = 0;

    if (!buff_len)
        buff_len = INT_MAX;

    while (*format != '\0')
    {
        size_t maxrem = buff_len - written;
        if (format[0] != '%' || format[1] == '%')
        {
            if (format[0] == '%')
                format++;
            size_t amount = 1;
            while (format[amount] && format[amount] != '%')
                amount++;
            if (maxrem < amount)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }
            
            for (size_t i = 0; i < amount; i++)
                buff[written + i] = format[i];
            
            format += amount;
            written += amount;
            continue;
        }

        const char* format_begun_at = format++;
        
        if (*format == 'c' || *format == 'C')
        {
            bool uppercase = *format == 'C';
            format++;
            char c = (char) va_arg(parameters, int); // char -> int.
            if (!maxrem)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }

            buff[written] = uppercase && c >= 'a' && c <= 'z' ? c - 32 : c;
            written++;
        }
        else if (*format == 's' || *format == 'S')
        {   
            bool uppercase = *format == 'S';
            format++;
            const char* str = va_arg(parameters, const char*);
            size_t len = strlen(str);
            if (maxrem < len)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }

            for (size_t i = 0; i < len; i++)
                buff[written + i] = uppercase && str[i] >= 'a' && str[i] <= 'z' ? str[i] - 32 : str[i];
               
            written += len;
        }
        else if (*format == 'd')
        {
            format++;
            int v = va_arg(parameters, int);

            char digits[12]; // max digits size in int decimal.
            itoa(v, digits, sizeof(digits), DECIMAL);
            
            size_t len = strlen(digits);
            if (maxrem < len)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }

            for (size_t i = 0; i < len; i++)
                 buff[written + i] = digits[i];

            written += len;
        }
        else if (*format == 'b' || *format == 'B')
        {
            bool uppercase = *format == 'B';
            format++;
            int v = va_arg(parameters, bool);
            
            const char* str = v ? "true" : "false";

            size_t len = strlen(str);
            if (maxrem < len)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }
            
            for (size_t i = 0; i < len; i++)
                buff[written + i] = 
                    uppercase ? str[i] - 32 : str[i];

            written += len;
        }
        else if ((*format == 'l' && format[1] == 'l' && (format[2] == 'x' || format[2] == 'X')) || (*format == 'x' || *format == 'X'))
        {
            // Vérifier pour %llx ou %llX (long long)
            bool is_long_long = (*format == 'l' && format[1] == 'l' && (format[2] == 'x' || format[2] == 'X'));
            bool uppercase;
            
            if (is_long_long)
            {
                // %llx ou %llX : long long unsigned
                uppercase = (format[2] == 'X');
                format += 3; // Consommer "llx" ou "llX"
                unsigned long long v = va_arg(parameters, unsigned long long);
                
                char digits[17]; // max digits size in long long hexadecimal (16 digits + null terminator).
                lltoa(v, digits, sizeof(digits), HEXADECIMAL);

                size_t len = strlen(digits);
                if (maxrem < len)
                {
                    // TODO: Implement OVERFLOW.
                    return EOF;
                }

                for (size_t i = 0; i < len; i++)
                    buff[written + i] = uppercase && digits[i] >= 'a' && digits[i] <= 'z' ? digits[i] - 32 : digits[i];

                written += len;
            }
            else
            {
                // %x ou %X : int
                uppercase = (*format == 'X');
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                
                // 8 hex digits for 32-bit values + null terminator.
                char digits[9];
                lltoa((unsigned long long) v, digits, sizeof(digits), HEXADECIMAL);

                size_t len = strlen(digits);
                if (maxrem < len)
                {
                    // TODO: Implement OVERFLOW.
                    return EOF;
                }

                for (size_t i = 0; i < len; i++)
                    buff[written + i] = uppercase && digits[i] >= 'a' && digits[i] <= 'z' ? digits[i] - 32 : digits[i];

                written += len;
            }
        }
        else
        {
            format = format_begun_at;
            size_t len = strlen(format);
            if (maxrem < len)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }
            
            for (size_t i = 0; i < len; i++)
                buff[written + i] = format[i];

            written += len;
            format += len;
        }
    }

    return written;
}

int printf(const char* __restrict format, ...)
{
    int result = EOF;
    /* TODO: find an algorithm to determine the ideal buffer length. */
    size_t len = 255;

    char buf[len];
    memset(buf, '\0', len);

    va_list parameters;
    va_start(parameters, format);

    result = __printf(buf, len, format, parameters);

    size_t output_len = strlen(buf);
    for (size_t i = 0; i < output_len; i++)
        putc(buf[i]);

#if defined(__THEOS_KERNEL) && defined(THEOS_ENABLE_KDEBUG)
    for (size_t i = 0; i < output_len; i++)
        kdebug_putc(buf[i]);
#endif

    va_end(parameters);

    return result;
}

int sprintf(char* str, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = __printf(str, NULL, format, parameters);

    va_end(parameters);

    return result;
}

int snprintf(char* str, size_t size, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = __printf(str, size, format, parameters);

    va_end(parameters);

    return result;
}

char* itoa(int value, char* buf, size_t length, unsigned int base)
{
    if (base < 2 || base > 36)
        return buf;

    int v = value;
    char digits[length];

    size_t index = 0;
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        if (base == 2)
        {
            digits[index++] = '0' + (v & 1);
            v >>= 1;
        }
        else
        {
            int digit = v % base;
            if (digit <= 9)
                digits[index++] = '0' + digit;
            else
                digits[index++] = 'a' + (digit - 10);
            v /= base;
        }
    }
    

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i]; // Ugly but work...

    buf[index] = '\0';
    return buf;
}

char* lltoa(unsigned long long value, char* buf, size_t length, unsigned int base)
{
    if (base < 2 || base > 36)
        return buf;

    unsigned long long v = value;
    char digits[length];

    size_t index = 0;
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        if (base == 2)
        {
            digits[index++] = '0' + (v & 1);
            v >>= 1;
        }
        else
        {
            unsigned long long digit = v % base;
            if (digit <= 9)
                digits[index++] = '0' + digit;
            else
                digits[index++] = 'a' + (digit - 10);
            v /= base;
        }
    }
    

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i]; // Ugly but work...

    buf[index] = '\0';
    return buf;
}
