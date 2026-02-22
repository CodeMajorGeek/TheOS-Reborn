#include <stdio.h>
#include <syscall.h>

void main(void)
{
    printf("Hello World from TheApp (ring3)\n");

    for (;;)
        (void) sys_sleep_ms(1000);
}
