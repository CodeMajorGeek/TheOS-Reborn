#include <stdio.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

static bool print(const char* data, size_t length, bool uppercase)
{
    unsigned char* bytes = (unsigned char*) data;
    for (size_t i = 0; i < length; i++)
    {
        if (uppercase && bytes[i] >= 'a' && bytes[i] <= 'z')
            bytes[i] -= 32; // lower case -> upper case.
        if (putchar(bytes[i]) == EOF)
            return false;
    }
    return true;
}

int putchar(int c)
{
    TTY_putc(c);
    return (char) c;
}

int puts(const char *s)
{
    TTY_puts(s);
    return 1;
}

int printf(const char* restrict format, ...)
{
    va_list parameters;
    va_start(parameters, format);

    int written = 0;

    while (*format != '\0')
    {
        size_t maxrem = INT_MAX - written;
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
            if (!print(format, amount, false))
                return EOF;
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
            if (!print(&c, sizeof(c), uppercase))
                return EOF;
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
            if (!print(str, len, uppercase))
                return EOF;
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
            if (!print(digits, len, false))
                return EOF;
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
            if (!print(str, len, uppercase))
                return EOF;
            written += len;
        }
        else if (*format == 'h' || *format == 'H')
        {
            bool uppercase = *format == 'H';
            format++;
            int v = va_arg(parameters, bool);
            
            char digits[7]; // max digits size in int hexadecimal.
            itoa(v, digits, sizeof(digits), HEXADECIMAL);

            size_t len = strlen(digits);
            if (maxrem < len)
            {
                // TODO: Implement OVERFLOW.
                return EOF;
            }
            if (!print(digits, len, uppercase))
                return EOF;
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
            if (!print(format, len, false))
                return EOF;
            written += len;
            format += len;
        }
    }
    va_end(parameters);
    return written;
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