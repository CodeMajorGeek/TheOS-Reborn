#include <Device/APIC.h>

bool APIC_check(void)
{
    uint32_t unused, edx;
    cpuid(1, &unused, &edx);

    return edx & CPUID_FEAT_EDX_APIC;
}

void APIC_set_base(uintptr_t apic)
{
    uint32_t edx = 0;
    uint32_t eax = (apic & 0xFFFFF0000) | IA32_APIC_BASE_MSR_ENABLE;

    MSR_set(IA32_APIC_BASE_MSR, eax, edx);
}

uintptr_t APIC_get_base(void)
{
    uint32_t eax, edx;
    MSR_get(IA32_APIC_BASE_MSR, &eax, &edx);

    return (eax & 0xFFFFF000);
}

void APIC_enable(void)
{
    APIC_set_base(APIC_get_base()); // Hardware enable the local APIC if it wasn't enabled yet.


}