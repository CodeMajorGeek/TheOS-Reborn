#include <stdio.h>


#ifndef THEOS_LIBC_BUILD
// We are building stdio as kernel

#define PRINTK_COM 1
#define PRINTK_TTY 2

#ifndef __THEOS_PRINTK_DEV
#define __THEOS_PRINTK_DEV PRINTK_TTY
#endif

#if __THEOS_PRINTK_DEV != PRINTK_COM && __THEOS_PRINTK_DEV != PRINTK_TTY
#error Error: __THEOS_PRINTK_DEV is not defined properly !
#endif

// Allows us to set the output device as a compilation parameter (See Kernel/CMakeLists.txt:33 )
#if __THEOS_PRINTK_DEV == PRINTK_TTY
#include <Device/TTY.h>
#define DEV_putc TTY_putc
#define DEV_puts TTY_puts
#elif __THEOS_PRINTK_DEV == PRINTK_COM
#include <Device/COM.h>
#define DEV_putc COM_putc
#define DEV_puts COM_puts
#endif

int putchar(int c)
{
    DEV_putc((char)c);

    return (char)c;
}

int puts(const char *s)
{
    DEV_puts(s);
    return 1;
}

#endif