#include <CPU/APIC.h>
#include <CPU/IO.h>

#include <cpuid.h>
#include <stdio.h>

static uint8_t APIC_lapic_IDs[256] = { 0 }; // CPU core Local APIC IDs.
static uint8_t APIC_num_core = 0;           // Number of cores detected.
static uint64_t APIC_IO_ptr = 0;            // Pointer to the IO APIC MMIO registers.
static uint64_t APIC_local_ptr = 0;         // Pointer to the Local APIC MMIO registers.

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

uint64_t APIC_local_read(uint64_t offset)
{
    return *((volatile uint32_t*) ((volatile uint64_t) APIC_local_ptr + offset));
}

void APIC_local_write(uint64_t offset, uint64_t value)
{
    *((volatile uint64_t*) ((volatile uint64_t) APIC_local_ptr + offset)) = value;
}

void APIC_enable(void)
{
    APIC_disable_PIC_mode();
    APIC_set_base(APIC_get_base()); // Hardware enable the local APIC if it wasn't enabled yet.

    APIC_local_write(APIC_SPURIOUS_VEC_REG, APIC_local_read(APIC_SPURIOUS_VEC_REG) | 0x100);
}

void APIC_detect_cores(APIC_MADT_t* madt)
{
    uint32_t madt_size = madt->SDT_header.length;
    void* madt_end = (void*) madt + madt_size;
    for (APIC_MADT_record_t* record = madt->records; record < madt_end; record += record->length)
    {
        void* record_content = record + sizeof (APIC_MADT_record_t);
        switch (record->type)
        {
            case APIC_PROCESSOR_LOCAL1_TYPE: // Found Processor Local APIC.
                APIC_proc_local1_t* lapic = (APIC_proc_local1_t*) record_content;
                if ((lapic->flags & APIC_PROCESSOR_ENABLED) || (lapic->flags & APIC_ONLINE_CAPABLE))
                    APIC_lapic_IDs[APIC_num_core++] = lapic->APIC_ID;
                break;
            case APIC_IO_TYPE: // Found IOAPIC.
                APIC_IO_t* ioapic = (APIC_IO_t*) record_content;
                APIC_IO_ptr = ioapic->IO_APIC_address;
                break;
            case APIC_LOCAL_ADDR_OVERRIDE_TYPE: // Found 64 bit LAPIC.
                APIC_local_addr_override_t* lapic_addr = (APIC_local_addr_override_t*) record_content;
                APIC_local_ptr = lapic_addr->phys_addr;
                break;
        }
    }
}

void APIC_disable_PIC_mode(void)
{
    IO_outb(APIC_IMCR_CTRL_REG, APIC_IMCR_ACCESS);
    IO_outb(APIC_IMCR_DATA_REG, APIC_FORCE_NMI_INTR_SIG);
}

void APIC_init(void)
{
    APIC_MADT_t* madt = (APIC_MADT_t*) ACPI_get_table(ACPI_APIC_SIGNATURE);
    APIC_local_ptr = madt->lapic_ptr;

    APIC_detect_cores(madt);

    printf("Found %d cores, IOAPIC 0x%H, LAPIC 0x%H, Processor IDs:", APIC_num_core, APIC_IO_ptr, APIC_local_ptr);
    for(int i = 0; i < APIC_num_core; i++)
        printf(" %d", APIC_lapic_IDs[i]);
    printf("\n");
}

uint64_t APIC_get_local_register(void)
{
    return APIC_local_ptr;
}

uint32_t APIC_read_IO(void* ioapicaddr, uint32_t reg)
{
   uint32_t volatile* ioapic = (uint32_t volatile*) ioapicaddr;
   ioapic[0] = (reg & 0xff);
   return ioapic[4];
}
 
void APIC_write_IO(void* ioapicaddr, uint32_t reg, uint32_t value)
{
   uint32_t volatile *ioapic = (uint32_t volatile *) ioapicaddr;
   ioapic[0] = (reg & 0xff);
   ioapic[4] = value;
}