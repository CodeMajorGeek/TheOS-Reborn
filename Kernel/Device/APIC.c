#include <Device/APIC.h>

bool APIC_check(void)
{
    uint32_t unused, edx;
    cpuid(1, &unused, &edx);

    return edx & CPUID_FEAT_EDX_APIC;
}