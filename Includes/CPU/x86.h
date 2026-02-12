#ifndef _X86_H
#define _X86_H

#include <stdint.h>

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

static inline uint64_t x86_read_cr0(void)
{
    uint64_t value = 0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void x86_write_cr0(uint64_t value)
{
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline uint64_t x86_read_cr4(void)
{
    uint64_t value = 0;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void x86_write_cr4(uint64_t value)
{
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(value) : "memory");
}

static inline void x86_set_ts(void)
{
    x86_write_cr0(x86_read_cr0() | (1ULL << 3));
}

static inline void x86_clear_ts(void)
{
    __asm__ __volatile__("clts");
}

static inline uint64_t x86_xgetbv(uint32_t index)
{
    uint32_t eax = 0;
    uint32_t edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t) edx << 32) | eax;
}

static inline void x86_xsetbv(uint32_t index, uint64_t value)
{
    uint32_t eax = (uint32_t) value;
    uint32_t edx = (uint32_t) (value >> 32);
    __asm__ __volatile__("xsetbv" : : "c"(index), "a"(eax), "d"(edx));
}

#endif
