#ifndef _X86_H
#define _X86_H

static inline void cli(void)
{
    __asm__ __volatile__("cli");
}

__attribute__((__noreturn__)) static inline void halt(void) 
{
    __asm__ __volatile__("hlt");

    for(;;)
        ;

    __builtin_unreachable();
}

#endif