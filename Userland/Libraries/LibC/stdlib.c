#include <stdlib.h>

#include <Debug/Spinlock.h>
#include <CPU/x86.h>

#include <stdbool.h>
#include <stdio.h>

void panic(char* s)
{
    uintptr_t pcs[10];

    cli();
    printf("PANIC on CPU !\n");
    printf(s);

    // TODO: implement proc stacktrace here.

    printf("\nSTACK:\n");
    get_caller_pcs(&s, pcs);
    for (int i = 0; i < 10 && pcs[i] != 0x0; i++)
        printf(" [%d] %p\n", i, pcs[i]);

    printf("HLT\n");
    halt();

     __builtin_unreachable();
}

void abort(void)
{
    cli();

    printf("Kernel Abort !");

    halt();
    __builtin_unreachable();
}