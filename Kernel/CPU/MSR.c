#include <CPU/MSR.h>

uint64_t MSR_get(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));

    return (((uint64_t) hi) << 32) | lo;
}

void MSR_set(uint32_t msr, uint64_t value)
{
    __asm__ __volatile__("wrmsr" : : "a"((uint32_t) value), "d"((uint32_t) (value >> 32)), "c"(msr));
}