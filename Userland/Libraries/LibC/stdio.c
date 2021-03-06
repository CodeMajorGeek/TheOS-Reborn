#include <stdio.h>

#include <Device/TTY.h>

#include <stdint.h>
#include <limits.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

int putc(int c)
{
    TTY_putc(c);
    return (char) c;
}

int puts(const char *s)
{
    TTY_puts(s);
    return 1;
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
                    uppercase ? str[i] - 32 : format[i];

            written += len;
        }
        else if (*format == 'x' || *format == 'X')
        {
            bool uppercase = *format == 'X';
            format++;
            int v = va_arg(parameters, int);
            
            char digits[7]; // max digits size in int hexadecimal.
            itoa(v, digits, sizeof(digits), HEXADECIMAL);

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
    
    for (int i = 0; i < strlen(buf); i++)
        putc(buf[i]);

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