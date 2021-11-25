#include <stdlib.h>

void abort(void)
{

    TTY_puts("kernel Abort !");

    __asm__ __volatile__("cli; hlt"); // Comletely hangs the computer !
    __builtin_unreachable();
}