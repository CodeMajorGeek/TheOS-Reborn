#include <Device/APIC.h>

#include <CPU/MSR.h>
#include <CPU/IO.h>

#include <cpuid.h>
#include <stdio.h>

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

void APIC_detect_cores(APIC_MADT_t* madt)
{
    APIC_MADT_record_t* record;
    for (uint32_t record_offset = 0; record_offset < (madt->SDT_header.length - sizeof (ACPI_SDT_header_t)); record_offset += record->length)
    {
        record = (APIC_MADT_record_t*) (madt->records + record_offset);
        
        switch (record->type)
        {
            case APIC_PROCESSOR_LOCAL1_TYPE:
                printf("Found processor local APIC !\n");
                break;
            case APIC_IO_TYPE:
                printf("Found IO APIC !\n");
                break;
            case APIC_IO_INT_SOURCE_OVERRIDE_TYPE:
                printf("Found IO interupt source override APIC !\n");
                break;
            case APIC_IO_NMASK_INT_SOURCE_TYPE:
                printf("Found IO not maskable interupt source APIC !\n");
                break;
            case APIC_LOCAL_NMASK_INT_TYPE:
                printf("Found local not maskable interupt APIC !\n");
                break;
            case APIC_LOCAL_ADDR_OVERRIDE_TYPE:
                printf("Found local address override APIC !\n");
                break;
            case APIC_PROCESSOR_LOCAL2_TYPE:
                printf("Found processor local APIC 2 !\n");
                break;
        }

        
    }
}

void APIC_init(void)
{
    APIC_MADT_t* madt = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);

    APIC_detect_cores(madt);

}