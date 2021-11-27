#include <stdio.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

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