#include <CPU/APIC.h>

#include <Memory/VMM.h>
#include <Memory/PMM.h>
#include <CPU/IO.h>

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

static uint8_t APIC_lapic_IDs[256] = { 0 }; // CPU core Local APIC IDs.
static uint8_t APIC_num_core = 0;           // Number of cores detected.

static APIC_IO_t APIC_IOs[256] = { 0 };     // IOAPIC descriptors.
static uint8_t APIC_IO_num = 0;             // Number of IOAPICs detected. 

static uint64_t APIC_local_ptr = 0;         // Pointer to the Local APIC MMIO registers.

static bool APIC_enabled = false;

static uint8_t APIC_IRQ_overrides[256];

bool APIC_check(void)
{
    uint32_t unused, edx;
    cpuid(1, &unused, &edx);

    APIC_enabled = edx & CPUID_FEAT_EDX_APIC;

    return APIC_enabled;
}

void APIC_enable(void)
{
    APIC_disable_PIC_mode();
    APIC_set_base(APIC_get_base()); // Hardware enable the local APIC if it wasn't enabled yet.

    APIC_local_write(APIC_DFR, 0xffffffff);

    uint32_t ldrval = APIC_local_read(APIC_LDR);
    ldrval &= 0x0ffffff;
    ldrval |= 1;

    APIC_local_write(APIC_LDR, ldrval);
    APIC_local_write(APIC_LVT_TMR, APIC_DISABLE);
    APIC_local_write(APIC_LVT_PERF, APIC_NMI);
    APIC_local_write(APIC_LVT_LINT0, APIC_DISABLE);
    APIC_local_write(APIC_LVT_LINT1, APIC_DISABLE);
    APIC_local_write(APIC_TASKPRIOR, 0);

    asm volatile (
                "movq $0x1b,%%rcx\n"
                "rdmsr\n"
                "bts $11,%%rax\n"
                "wrmsr"
                  :::"rax");
    APIC_local_write(APIC_SPURIOUS, 0x1ff);

    // APIC_local_write(APIC_SPURIOUS, APIC_local_read(APIC_SPURIOUS) | 0x100);
}

void APIC_detect_cores(APIC_MADT_t* madt)
{
    for (size_t i = 0; i < sizeof(APIC_IRQ_overrides); i++)
        APIC_IRQ_overrides[i] = i;


    APIC_local_ptr = madt->lapic_ptr;
    VMM_map_page((uintptr_t) APIC_local_ptr, (uintptr_t) APIC_local_ptr);

    size_t madt_size = madt->SDT_header.length;
    uintptr_t madt_end = (uintptr_t) madt + madt_size;
    
    uintptr_t current_ptr = (uintptr_t) madt->records;
    while (current_ptr < madt_end)
    {
        uint8_t type = *((uint8_t*) current_ptr);
        uint8_t length = *((uint8_t*) (current_ptr + 1));

        switch (type)
        {
            case APIC_PROCESSOR_LOCAL1_TYPE:
                if (*((uint32_t*) (current_ptr + 4)) & 1)
                    APIC_lapic_IDs[APIC_num_core++] = *(uint8_t*) (current_ptr + 3);
                break;
            case APIC_IO_TYPE:
                APIC_IO_t* io = &APIC_IOs[APIC_IO_num];

                io->id = *(uint8_t*) (current_ptr + 2);
                io->ptr = *(uint32_t*) (current_ptr + 4);

                VMM_map_page((uintptr_t) io->ptr, (uintptr_t) io->ptr);
                io->irq_base = *((uint32_t*) (current_ptr + 8));
                io->irq_end =  io->irq_base + ((APIC_IO_read(APIC_IO_num, 0x1) >> 16) & 0xFF);

                APIC_IO_num++;
                break;
            case APIC_IO_INT_SOURCE_OVERRIDE_TYPE:
                APIC_IRQ_overrides[*(uint8_t*) (current_ptr + 3)] = *(uint32_t*) (current_ptr + 4);
                break;
            default:
                { }
        }

        current_ptr += length;
    }

}

void APIC_disable_PIC_mode(void)
{
    IO_outb(APIC_IMCR_CTRL_REG, APIC_IMCR_ACCESS);
    IO_outb(APIC_IMCR_DATA_REG, APIC_FORCE_NMI_INTR_SIG);
}

void APIC_init(APIC_MADT_t* madt)
{
    APIC_detect_cores(madt);

    printf("Found %d cores, LAPIC 0x%X, Processor IDs:", APIC_num_core, APIC_local_ptr);
    for(int i = 0; i < APIC_num_core; i++)
        printf(" %d", APIC_lapic_IDs[i]);
    printf("\n");

    printf("Found %d IOAPIC:\n", APIC_IO_num);
    for(int i = 0; i < APIC_IO_num; i++)
        printf("\tID: %d, Ptr: 0x%X, IRQ base: %d, IRQ end: %d !\n", APIC_IOs[i].id, APIC_IOs[i].ptr, APIC_IOs[i].irq_base, APIC_IOs[i].irq_end);

    printf("Redirection entries found:\n");
    for (int i = 0; i < sizeof(APIC_IRQ_overrides); i++)
        if (i != APIC_IRQ_overrides[i]) 
            printf("\t %d => %d\n", i, APIC_IRQ_overrides[i]);
}

uintptr_t APIC_get_base(void)
{
    uint64_t value = MSR_get(IA32_APIC_BASE_MSR);
    return (value & 0xFFFFF000);
}

void APIC_set_base(uintptr_t apic)
{
    MSR_set(IA32_APIC_BASE_MSR, (apic & 0xFFFFF0000) | IA32_APIC_BASE_MSR_ENABLE);
}

uint64_t APIC_local_read(uint64_t offset)
{
    return *((volatile uint32_t*) ((volatile uint64_t) APIC_local_ptr + offset));
}

void APIC_local_write(uint64_t offset, uint64_t value)
{
    *((volatile uint64_t*) ((volatile uint64_t) APIC_local_ptr + offset)) = value;
}

uint32_t APIC_IO_read(uint8_t index, uint32_t reg)
{
   uint32_t volatile* ioapic = (uint32_t volatile*) APIC_IOs[index].ptr;
   ioapic[0] = (reg & 0xff);
   return ioapic[4];
}
 
void APIC_IO_write(uint8_t index, uint32_t reg, uint32_t value)
{
   uint32_t volatile *ioapic = (uint32_t volatile *) APIC_IOs[index].ptr;
   ioapic[0] = (reg & 0xff);
   ioapic[4] = value;
}

bool APIC_is_enabled(void)
{
    return APIC_enabled;
}

void APIC_register_IRQ_vector(int vec, int irq, bool disable)
{
    uint8_t real_IRQ = APIC_IRQ_overrides[irq];

    uint8_t IOAPIC_idx;
    for (IOAPIC_idx = 0; IOAPIC_idx < APIC_IO_num; IOAPIC_idx++)
        if (real_IRQ >= APIC_IOs[IOAPIC_idx].irq_base && real_IRQ <= APIC_IOs[IOAPIC_idx].irq_end)
        {
            real_IRQ -= APIC_IOs[IOAPIC_idx].irq_base;
            break;
        }

    uint32_t IO_lo = vec;
    uint32_t IO_hi = APIC_lapic_IDs[0] << 24;

    if (disable)
        IO_lo |= (1 << 16);

    uint32_t reg = 0x10 + (real_IRQ * 2);
    APIC_IO_write(IOAPIC_idx, reg, IO_lo);
    APIC_IO_write(IOAPIC_idx, reg + 1, IO_hi);
}

void APIC_send_EOI(void)
{
    APIC_local_write(APIC_EOI, 0);
}