#include <CPU/MSR.h>

void MSR_get(uint32_t msr, uint32_t* lo, uint32_t* hi)
{
    __asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

void MSR_set(uint32_t msr, uint32_t lo, uint32_t hi)
{
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}