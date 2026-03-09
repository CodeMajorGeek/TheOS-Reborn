#include <stdlib.h>

#include <Debug/Spinlock.h>
#include <CPU/x86.h>

#include <stdbool.h>
#include <stdio.h>

#ifdef __THEOS_KERNEL
#include <CPU/SMP.h>
#include <Debug/KDebug.h>
#include <Device/TTY.h>
#else
#include <syscall.h>
#include <signal.h>
#endif

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
#ifdef __THEOS_KERNEL
    static spinlock_t abort_lock = { 0 };
    static volatile bool abort_broadcast_done = false;

    cli();

    spin_lock(&abort_lock);
    bool first = !abort_broadcast_done;
    if (first)
    {
        abort_broadcast_done = true;
        /* Log to framebuffer/TTY and serial before stopping everything. */
        TTY_puts("Kernel Abort !\n");
        kdebug_puts("[ABORT] Kernel Abort ! Halting all CPUs.\n");
        /* Stop all other CPUs as quickly as possible. */
        (void) SMP_send_ipi_to_others(SMP_IPI_VECTOR_PANIC);
    }
    spin_unlock(&abort_lock);

    for (;;)
        halt();
#else
    printf("abort() called, terminating process.\n");
    sys_exit(128 + SIGABRT);
#endif
    __buitin_unrecheable();
}