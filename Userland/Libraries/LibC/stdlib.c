#include <stdlib.h>

#include <Device/TTY.h>

#include <stdbool.h>

void abort(void)
{

    TTY_puts("Kernel Abort !");

    __asm__ __volatile__("cli; hlt"); // Completely hangs the computer !
    __builtin_unreachable();
}