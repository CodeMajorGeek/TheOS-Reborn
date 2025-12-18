#ifndef _X86_H
#define _X86_H

static inline void cli(void)
{
    __asm__ __volatile__("cli");
}

static inline void sti(void)
{
    __asm__ __volatile__ ("sti");
}

__attribute__((__noreturn__)) static inline void halt(void) 
{
    __asm__ __volatile__("hlt");

    for(;;)
        ;

    __builtin_unreachable();
}

#endif