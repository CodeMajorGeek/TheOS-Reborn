#include <Device/APIC.h>

#include <CPU/MSR.h>
#include <CPU/IO.h>

#include <cpuid.h>

bool APIC_check(void)
{
    uint32_t unused, edx;
    cpuid(1, &unused, &edx);

    return edx & CPUID_FEAT_EDX_APIC;
}

void APIC_set_base(uintptr_t apic)
{
    MSR_set(IA32_APIC_BASE_MSR, (apic & 0xFFFFF0000) | IA32_APIC_BASE_MSR_ENABLE);
}

uintptr_t APIC_get_base(void)
{
    uint64_t value = MSR_get(IA32_APIC_BASE_MSR);
    return (value & 0xFFFFF000);
}

void APIC_enable(void)
{
    APIC_set_base(APIC_get_base()); // Hardware enable the local APIC if it wasn't enabled yet.


}