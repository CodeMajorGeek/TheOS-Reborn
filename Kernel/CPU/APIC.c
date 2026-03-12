#include <CPU/APIC.h>
#include <CPU/APIC_private.h>

#include <CPU/ISR.h>
#include <CPU/Syscall.h>
#include <Device/HPET.h>
#include <Device/PIT.h>
#include <Device/TTY.h>
#include <Memory/VMM.h>
#include <CPU/IO.h>
#include <Debug/KDebug.h>
#include <Task/Task.h>

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

static APIC_runtime_state_t APIC_runtime_state;

static inline void APIC_cpuid_leaf1(uint32_t* eax,
                                    uint32_t* ebx,
                                    uint32_t* ecx,
                                    uint32_t* edx)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1U), "c"(0U));
    if (eax)
        *eax = a;
    if (ebx)
        *ebx = b;
    if (ecx)
        *ecx = c;
    if (edx)
        *edx = d;
}

static bool APIC_x2apic_write_icr(uint8_t apic_id, uint32_t icr_low)
{
    uint64_t icr = ((uint64_t) apic_id << 32) | (uint64_t) icr_low;
    MSR_set(IA32_X2APIC_MSR_ICR, icr);
    return APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT);
}

static uintptr_t APIC_mmio_virt(uintptr_t phys)
{
    return VMM_MMIO_VIRT(phys);
}

static void APIC_map_mmio_page(uintptr_t phys)
{
    uintptr_t page_phys = phys & ~(uintptr_t) 0xFFFULL;
    VMM_map_mmio_uc_page(APIC_mmio_virt(page_phys), page_phys);
}

bool APIC_check(void)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    APIC_cpuid_leaf1(&eax, &ebx, &ecx, &edx);
    (void) ebx;

    APIC_runtime_state.capability.supported = (edx & CPUID_FEAT_EDX_APIC) != 0;
    APIC_runtime_state.capability.x2apic_supported = (ecx & CPUID_FEAT_ECX_X2APIC) != 0;
    APIC_runtime_state.capability.x2apic_enabled = false;
    APIC_runtime_state.capability.enabled = false;

    return APIC_runtime_state.capability.supported;
}

void APIC_enable(void)
{
    if (!APIC_runtime_state.capability.supported)
        return;

    if (APIC_runtime_state.local_phys == 0)
        APIC_runtime_state.local_phys = APIC_get_base();
    if (APIC_runtime_state.local_phys == 0)
        return;

    APIC_disable_PIC_mode();
    uint64_t apic_base = MSR_get(IA32_APIC_BASE_MSR);
    apic_base &= ~0xFFFFFFFFFFFFF000ULL;
    apic_base |= (APIC_runtime_state.local_phys & 0xFFFFFFFFFFFFF000ULL);
    apic_base |= IA32_APIC_BASE_MSR_ENABLE;

    bool want_x2apic = APIC_runtime_state.capability.x2apic_supported;
#if THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL == 0
    if (APIC_runtime_state.topology.num_core > 1)
        want_x2apic = false;
#endif

    if (want_x2apic)
        apic_base |= IA32_APIC_BASE_MSR_X2APIC;
    else
        apic_base &= ~((uint64_t) IA32_APIC_BASE_MSR_X2APIC);
    MSR_set(IA32_APIC_BASE_MSR, apic_base);

    APIC_runtime_state.capability.x2apic_enabled = (MSR_get(IA32_APIC_BASE_MSR) & IA32_APIC_BASE_MSR_X2APIC) != 0;
    if (!APIC_runtime_state.capability.x2apic_enabled)
    {
        APIC_map_mmio_page(APIC_runtime_state.local_phys);
        APIC_runtime_state.local_ptr = APIC_mmio_virt(APIC_runtime_state.local_phys);

        APIC_local_write(APIC_DFR, 0xffffffff);
        uint32_t ldrval = APIC_local_read(APIC_LDR);
        ldrval &= 0x0ffffff;
        ldrval |= 1;
        APIC_local_write(APIC_LDR, ldrval);
    }
    else
    {
        APIC_runtime_state.local_ptr = 0;
    }
    
    APIC_local_write(APIC_LVT_TMR, APIC_DISABLE);
    APIC_local_write(APIC_LVT_PERF, APIC_NMI);
    APIC_local_write(APIC_LVT_LINT0, APIC_DISABLE);
    APIC_local_write(APIC_LVT_LINT1, APIC_DISABLE);
    APIC_local_write(APIC_TASKPRIOR, 0);

    // Use a deterministic spurious vector (0xFF). We have a stub for all 256 vectors.
    uint32_t spurious = APIC_local_read(APIC_SPURIOUS);
    spurious &= ~0xFFU;
    spurious |= 0xFFU;
    spurious |= APIC_SW_ENABLE;
    APIC_local_write(APIC_SPURIOUS, spurious);
    uint32_t current_lapic_id_raw = APIC_local_read(APIC_APICID);
    uint8_t current_lapic_id = APIC_runtime_state.capability.x2apic_enabled
                                 ? (uint8_t) current_lapic_id_raw
                                 : (uint8_t) (current_lapic_id_raw >> 24);
    if (!APIC_runtime_state.topology.io_route_lapic_valid)
    {
        APIC_runtime_state.topology.bsp_lapic_id = current_lapic_id;
        APIC_runtime_state.topology.io_route_lapic_id = current_lapic_id;
        APIC_runtime_state.topology.io_route_lapic_valid = true;
    }
    APIC_runtime_state.capability.enabled = true;
    kdebug_printf("[APIC] mode=%s\n", APIC_runtime_state.capability.x2apic_enabled ? "x2APIC" : "xAPIC");
    if (APIC_runtime_state.capability.x2apic_supported && !APIC_runtime_state.capability.x2apic_enabled && APIC_runtime_state.topology.num_core > 1 && !APIC_runtime_state.capability.x2apic_smp_skip_reported)
    {
        kdebug_puts("[APIC] x2APIC supported but disabled on SMP (set THEOS_ENABLE_X2APIC_SMP_EXPERIMENTAL=1 to force)\n");
        APIC_runtime_state.capability.x2apic_smp_skip_reported = true;
    }
    kdebug_printf("[APIC] current id raw=0x%X mapped=0x%X\n",
                  current_lapic_id_raw,
                  current_lapic_id);
}

void APIC_detect_cores(APIC_MADT_t* madt)
{
    if (madt == NULL)
        return;

    kdebug_printf("[APIC] MADT=%p len=%u\n", madt, madt->SDT_header.length);

    const size_t irq_override_count = sizeof(APIC_runtime_state.topology.irq_overrides) / sizeof(APIC_runtime_state.topology.irq_overrides[0]);
    for (size_t i = 0; i < irq_override_count; i++)
    {
        APIC_runtime_state.topology.irq_overrides[i] = i;
        APIC_runtime_state.topology.gsi_flags[i] = 0;
        APIC_runtime_state.topology.gsi_flags_valid[i] = false;
        APIC_runtime_state.topology.gsi_is_isa[i] = (i < 16);
    }

    APIC_runtime_state.topology.num_core = 0;
    APIC_runtime_state.topology.ioapic_count = 0;
    APIC_runtime_state.topology.io_route_lapic_valid = false;

    APIC_runtime_state.local_phys = madt->lapic_ptr;
    if (APIC_runtime_state.local_phys == 0)
        APIC_runtime_state.local_phys = APIC_get_base();
    if (APIC_runtime_state.local_phys != 0)
    {
        APIC_map_mmio_page(APIC_runtime_state.local_phys);
        APIC_runtime_state.local_ptr = APIC_mmio_virt(APIC_runtime_state.local_phys);
        APIC_runtime_state.topology.bsp_lapic_id = (uint8_t) (APIC_local_read(APIC_APICID) >> 24);
        APIC_runtime_state.topology.io_route_lapic_id = APIC_runtime_state.topology.bsp_lapic_id;
        APIC_runtime_state.topology.io_route_lapic_valid = true;
        kdebug_printf("[APIC] LAPIC base=0x%llX\n", (unsigned long long) APIC_runtime_state.local_phys);
    }

    size_t madt_size = madt->SDT_header.length;
    uintptr_t madt_end = (uintptr_t) madt + madt_size;
    
    uintptr_t current_ptr = (uintptr_t) madt->records;
    // MADT is a variable-length record stream; each record is self-describing.
    while (current_ptr < madt_end)
    {
        uint8_t type = *((uint8_t*) current_ptr);
        uint8_t length = *((uint8_t*) (current_ptr + 1));
        kdebug_printf("[APIC] rec type=%u len=%u ptr=0x%llX\n", type, length, current_ptr);
        if (length < 2 || (current_ptr + length) > madt_end)
            break;

        switch (type)
        {
            case APIC_PROCESSOR_LOCAL1_TYPE:
                if (length >= 8 && APIC_runtime_state.topology.num_core < sizeof(APIC_runtime_state.topology.lapic_ids) &&
                    (*((uint32_t*) (current_ptr + 4)) & 1))
                    APIC_runtime_state.topology.lapic_ids[APIC_runtime_state.topology.num_core++] = *(uint8_t*) (current_ptr + 3);
                break;
            case APIC_PROCESSOR_LOCAL2_TYPE:
                if (length >= 16 && APIC_runtime_state.topology.num_core < sizeof(APIC_runtime_state.topology.lapic_ids))
                {
                    uint32_t x2apic_id = *(uint32_t*) (current_ptr + 4);
                    uint32_t flags = *(uint32_t*) (current_ptr + 8);
                    if ((flags & APIC_PROCESSOR_ENABLED) != 0 || (flags & APIC_ONLINE_CAPABLE) != 0)
                    {
                        if (x2apic_id <= 0xFFU)
                        {
                            APIC_runtime_state.topology.lapic_ids[APIC_runtime_state.topology.num_core++] = (uint8_t) x2apic_id;
                        }
                        else
                        {
                            kdebug_printf("[APIC] x2APIC id=%u not representable in legacy 8-bit map, skipping\n",
                                          x2apic_id);
                        }
                    }
                }
                break;
            case APIC_IO_TYPE:
                if (length < 12 || APIC_runtime_state.topology.ioapic_count >= sizeof(APIC_runtime_state.topology.ioapics) / sizeof(APIC_runtime_state.topology.ioapics[0]))
                    break;

                kdebug_puts("[APIC] io rec start\n");
                APIC_IO_t* io = &APIC_runtime_state.topology.ioapics[APIC_runtime_state.topology.ioapic_count];

                io->id = *(uint8_t*) (current_ptr + 2);
                io->ptr = *(uint32_t*) (current_ptr + 4);
                kdebug_printf("[APIC] io raw id=%u ptr=0x%X\n", io->id, io->ptr);

                uintptr_t ioapic_page = (uintptr_t) io->ptr & 0xFFFFFFFFFFFFF000ULL;
                VMM_map_mmio_uc_page(APIC_mmio_virt(ioapic_page), ioapic_page);
                kdebug_puts("[APIC] io mapped\n");
                io->irq_base = *((uint32_t*) (current_ptr + 8));
                kdebug_printf("[APIC] io base=%u\n", io->irq_base);
                // QEMU's IOAPIC commonly exposes 24 redirection entries (0..23).
                // Avoid probing MMIO here during very early boot.
                io->irq_end = io->irq_base + 23;
                kdebug_printf("[APIC] IOAPIC id=%u ptr=0x%X base=%u end=%u\n", io->id, io->ptr, io->irq_base, io->irq_end);

                APIC_runtime_state.topology.ioapic_count++;
                break;
            case APIC_IO_INT_SOURCE_OVERRIDE_TYPE:
                if (length >= 10)
                {
                    uint8_t bus = *(uint8_t*) (current_ptr + 2);
                    uint8_t source_irq = *(uint8_t*) (current_ptr + 3);
                    uint32_t gsi = *(uint32_t*) (current_ptr + 4);
                    uint16_t flags = *(uint16_t*) (current_ptr + 8);

                    if ((size_t) source_irq < irq_override_count)
                        APIC_runtime_state.topology.irq_overrides[source_irq] = gsi;

                    if ((size_t) gsi < irq_override_count)
                    {
                        APIC_runtime_state.topology.gsi_flags[gsi] = flags;
                        APIC_runtime_state.topology.gsi_flags_valid[gsi] = true;
                        APIC_runtime_state.topology.gsi_is_isa[gsi] = (bus == 0);
                    }

                    kdebug_printf("[APIC] override bus=%u irq=%u=>gsi=%u flags=0x%X\n", bus, source_irq, gsi, flags);
                }
                break;
            case APIC_LOCAL_ADDR_OVERRIDE_TYPE:
                if (length >= 12)
                {
                    APIC_runtime_state.local_phys = *(uint64_t*) (current_ptr + 4);
                    APIC_map_mmio_page(APIC_runtime_state.local_phys);
                    APIC_runtime_state.local_ptr = APIC_mmio_virt(APIC_runtime_state.local_phys);
                    kdebug_printf("[APIC] LAPIC override=0x%llX\n", (unsigned long long) APIC_runtime_state.local_phys);
                }
                break;
            default:
                { }
        }

        current_ptr += length;
    }

    if (APIC_runtime_state.topology.num_core > 0 && APIC_runtime_state.topology.bsp_lapic_id == 0)
        APIC_runtime_state.topology.bsp_lapic_id = APIC_runtime_state.topology.lapic_ids[0];

    if (!APIC_runtime_state.topology.io_route_lapic_valid && APIC_runtime_state.topology.num_core > 0)
    {
        APIC_runtime_state.topology.io_route_lapic_id = APIC_runtime_state.topology.lapic_ids[0];
        APIC_runtime_state.topology.io_route_lapic_valid = true;
    }
}

void APIC_disable_PIC_mode(void)
{
    IO_outb(APIC_IMCR_CTRL_REG, APIC_IMCR_ACCESS);
    IO_outb(APIC_IMCR_DATA_REG, APIC_FORCE_NMI_INTR_SIG);
}

void APIC_init(APIC_MADT_t* madt)
{
    kdebug_puts("[APIC] init start\n");

    if (madt == NULL)
        return;

    APIC_detect_cores(madt);
    kdebug_printf("[APIC] detect done cores=%u ioapic=%u lapic=0x%llX\n",
                  APIC_runtime_state.topology.num_core,
                  APIC_runtime_state.topology.ioapic_count,
                  (unsigned long long) APIC_runtime_state.local_phys);

    printf("Found %d cores, LAPIC 0x%llX, Processor IDs:", APIC_runtime_state.topology.num_core, (unsigned long long) APIC_runtime_state.local_phys);
    for(int i = 0; i < APIC_runtime_state.topology.num_core; i++)
        printf(" %d", APIC_runtime_state.topology.lapic_ids[i]);
    printf("\n");

    printf("Found %d IOAPIC:\n", APIC_runtime_state.topology.ioapic_count);
    for(int i = 0; i < APIC_runtime_state.topology.ioapic_count; i++)
        printf("\tID: %d, Ptr: 0x%X, IRQ base: %d, IRQ end: %d !\n", APIC_runtime_state.topology.ioapics[i].id, APIC_runtime_state.topology.ioapics[i].ptr, APIC_runtime_state.topology.ioapics[i].irq_base, APIC_runtime_state.topology.ioapics[i].irq_end);

    printf("Redirection entries found:\n");
    const size_t irq_override_count = sizeof(APIC_runtime_state.topology.irq_overrides) / sizeof(APIC_runtime_state.topology.irq_overrides[0]);
    for (size_t i = 0; i < irq_override_count; i++)
        if (i != APIC_runtime_state.topology.irq_overrides[i]) 
            printf("\t %d => %d\n", (unsigned) i, APIC_runtime_state.topology.irq_overrides[i]);
}

uintptr_t APIC_get_base(void)
{
    uint64_t value = MSR_get(IA32_APIC_BASE_MSR);
    return (uintptr_t) (value & 0xFFFFFFFFFFFFF000ULL);
}

void APIC_set_base(uintptr_t apic)
{
    uint64_t current = MSR_get(IA32_APIC_BASE_MSR);
    uint64_t value = current & ~0xFFFFFFFFFFFFF000ULL;
    value |= (apic & 0xFFFFFFFFFFFFF000ULL);
    value |= IA32_APIC_BASE_MSR_ENABLE;
    MSR_set(IA32_APIC_BASE_MSR, value);
}

uint32_t APIC_local_read(uint32_t offset)
{
    if (APIC_runtime_state.capability.x2apic_enabled)
    {
        uint32_t msr = IA32_X2APIC_MSR_BASE + (offset >> 4);
        return (uint32_t) MSR_get(msr);
    }

    if (APIC_runtime_state.local_ptr == 0)
        return 0;

    return *((volatile uint32_t*) ((volatile uintptr_t) APIC_runtime_state.local_ptr + offset));
}

void APIC_local_write(uint32_t offset, uint32_t value)
{
    if (APIC_runtime_state.capability.x2apic_enabled)
    {
        uint32_t msr = IA32_X2APIC_MSR_BASE + (offset >> 4);
        /* Some x2APIC registers are write-only (e.g. EOI). */
        MSR_set(msr, (uint64_t) value);
        return;
    }

    if (APIC_runtime_state.local_ptr == 0)
        return;

    *((volatile uint32_t*) ((volatile uintptr_t) APIC_runtime_state.local_ptr + offset)) = value;
}

uint32_t APIC_IO_read(uint8_t index, uint32_t reg)
{
   if (index >= APIC_runtime_state.topology.ioapic_count || APIC_runtime_state.topology.ioapics[index].ptr == 0)
      return 0xFFFFFFFF;

   uintptr_t ioapic_virt = APIC_mmio_virt((uintptr_t) APIC_runtime_state.topology.ioapics[index].ptr);
   uint32_t volatile* ioapic = (uint32_t volatile*) ioapic_virt;
   ioapic[0] = (reg & 0xff);
   return ioapic[4];
}
 
void APIC_IO_write(uint8_t index, uint32_t reg, uint32_t value)
{
   if (index >= APIC_runtime_state.topology.ioapic_count || APIC_runtime_state.topology.ioapics[index].ptr == 0)
      return;

   uintptr_t ioapic_virt = APIC_mmio_virt((uintptr_t) APIC_runtime_state.topology.ioapics[index].ptr);
   uint32_t volatile* ioapic = (uint32_t volatile*) ioapic_virt;
   ioapic[0] = (reg & 0xff);
   ioapic[4] = value;
}

bool APIC_is_enabled(void)
{
    return APIC_runtime_state.capability.enabled;
}

bool APIC_is_x2apic_enabled(void)
{
    return APIC_runtime_state.capability.x2apic_enabled;
}

uint8_t APIC_get_current_lapic_id(void)
{
    if (!APIC_runtime_state.capability.x2apic_enabled && APIC_runtime_state.local_ptr == 0)
        return 0;

    if (APIC_runtime_state.capability.x2apic_enabled)
        return (uint8_t) APIC_local_read(APIC_APICID);

    return (uint8_t) (APIC_local_read(APIC_APICID) >> 24);
}

uint8_t APIC_get_bsp_lapic_id(void)
{
    if (APIC_runtime_state.topology.io_route_lapic_valid)
        return APIC_runtime_state.topology.io_route_lapic_id;

    return APIC_runtime_state.topology.bsp_lapic_id;
}

uint8_t APIC_get_core_count(void)
{
    return APIC_runtime_state.topology.num_core;
}

uint8_t APIC_get_core_id(uint8_t index)
{
    if (index >= APIC_runtime_state.topology.num_core)
        return 0xFF;

    return APIC_runtime_state.topology.lapic_ids[index];
}

void APIC_register_IRQ_vector(int vec, int irq, bool disable)
{
    const size_t irq_override_count = sizeof(APIC_runtime_state.topology.irq_overrides) / sizeof(APIC_runtime_state.topology.irq_overrides[0]);
    if (irq < 0 || (size_t) irq >= irq_override_count)
        return;

    APIC_register_GSI_vector(vec, APIC_runtime_state.topology.irq_overrides[irq], disable);
}

void APIC_register_GSI_vector(int vec, uint32_t gsi, bool disable)
{
    if (APIC_runtime_state.topology.ioapic_count == 0)
        return;

    uint32_t real_IRQ = gsi;
    uint8_t IOAPIC_idx = 0;
    bool found = false;
    for (IOAPIC_idx = 0; IOAPIC_idx < APIC_runtime_state.topology.ioapic_count; IOAPIC_idx++)
        if (real_IRQ >= APIC_runtime_state.topology.ioapics[IOAPIC_idx].irq_base && real_IRQ <= APIC_runtime_state.topology.ioapics[IOAPIC_idx].irq_end)
        {
            real_IRQ -= APIC_runtime_state.topology.ioapics[IOAPIC_idx].irq_base;
            found = true;
            break;
        }
    if (!found)
        return;

    uint32_t IO_lo = vec & 0xFFU;
    uint8_t lapic_id = APIC_runtime_state.topology.io_route_lapic_id;
    if (!APIC_runtime_state.topology.io_route_lapic_valid)
    {
        lapic_id = APIC_runtime_state.topology.bsp_lapic_id;
        if (lapic_id == 0 && APIC_runtime_state.topology.num_core > 0)
            lapic_id = APIC_runtime_state.topology.lapic_ids[0];
    }
    uint32_t IO_hi = ((uint32_t) lapic_id) << 24;

    bool active_low = false;
    bool level_triggered = false;
    uint16_t flags = 0;
    uint16_t polarity = APIC_MADT_INTI_POLARITY_CONFORMS;
    uint16_t trigger = APIC_MADT_INTI_TRIGGER_CONFORMS;
    bool is_isa = false;
    const size_t gsi_flag_count = sizeof(APIC_runtime_state.topology.gsi_flags) / sizeof(APIC_runtime_state.topology.gsi_flags[0]);

    if ((size_t) gsi < gsi_flag_count)
        is_isa = APIC_runtime_state.topology.gsi_is_isa[gsi];

    if ((size_t) gsi < gsi_flag_count && APIC_runtime_state.topology.gsi_flags_valid[gsi])
    {
        flags = APIC_runtime_state.topology.gsi_flags[gsi];
        polarity = (uint16_t) (flags & APIC_MADT_INTI_POLARITY_MASK);
        trigger = (uint16_t) ((flags & APIC_MADT_INTI_TRIGGER_MASK) >> APIC_MADT_INTI_TRIGGER_SHIFT);
    }

    // ACPI "conforms" follows the source bus. ISA defaults to active-high/edge.
    switch (polarity)
    {
        case APIC_MADT_INTI_POLARITY_ACTIVE_LOW:
            active_low = true;
            break;
        case APIC_MADT_INTI_POLARITY_ACTIVE_HIGH:
            active_low = false;
            break;
        case APIC_MADT_INTI_POLARITY_CONFORMS:
            active_low = false;
            break;
        case APIC_MADT_INTI_POLARITY_RESERVED:
        default:
            active_low = false;
            kdebug_printf("[APIC] warning: GSI %u has reserved polarity in flags=0x%X\n", gsi, flags);
            break;
    }

    switch (trigger)
    {
        case APIC_MADT_INTI_TRIGGER_LEVEL:
            level_triggered = true;
            break;
        case APIC_MADT_INTI_TRIGGER_EDGE:
            level_triggered = false;
            break;
        case APIC_MADT_INTI_TRIGGER_CONFORMS:
            level_triggered = false;
            break;
        case APIC_MADT_INTI_TRIGGER_RESERVED:
        default:
            level_triggered = false;
            kdebug_printf("[APIC] warning: GSI %u has reserved trigger in flags=0x%X\n", gsi, flags);
            break;
    }

    if (!is_isa && (polarity == APIC_MADT_INTI_POLARITY_CONFORMS || trigger == APIC_MADT_INTI_TRIGGER_CONFORMS))
        kdebug_printf("[APIC] warning: GSI %u uses 'conforms' on non-ISA source bus, keeping high/edge defaults\n", gsi);

    if (active_low)
        IO_lo |= APIC_IORED_POLARITY_LOW;
    if (level_triggered)
        IO_lo |= APIC_IORED_TRIGGER_LEVEL;

    if (disable)
        IO_lo |= APIC_IORED_MASK;

    // Program IOAPIC redirection as a 64-bit entry split across two 32-bit registers.
    uint32_t reg = 0x10 + (real_IRQ * 2);
    APIC_IO_write(IOAPIC_idx, reg, IO_lo);
    APIC_IO_write(IOAPIC_idx, reg + 1, IO_hi);
    kdebug_printf("[APIC] route GSI %u -> vec 0x%X (LAPIC %u) flags=0x%X pol=%s trig=%s%s\n",
                  gsi, vec, lapic_id, flags,
                  active_low ? "low" : "high",
                  level_triggered ? "level" : "edge",
                  disable ? " [masked]" : "");
}

bool APIC_startup_ap(uint8_t apic_id, uint8_t startup_vector)
{
    if (!APIC_runtime_state.capability.enabled)
        return false;

    if (startup_vector == 0)
        return false;

    if (!APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT))
        return false;

    if (APIC_runtime_state.capability.x2apic_enabled)
    {
        kdebug_printf("[APIC] x2APIC startup INIT apic_id=%u vec=0x%X\n", apic_id, startup_vector);
        if (!APIC_x2apic_write_icr(apic_id, APIC_ICR_DELIVERY_INIT | APIC_ICR_TRIGGER_LEVEL | APIC_ICR_LEVEL_ASSERT))
            return false;
        APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);

        if (!APIC_x2apic_write_icr(apic_id, APIC_ICR_DELIVERY_INIT | APIC_ICR_TRIGGER_LEVEL))
            return false;
        APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);

        uint32_t sipi_icr = APIC_ICR_DELIVERY_STARTUP | (uint32_t) startup_vector;
        kdebug_printf("[APIC] x2APIC startup SIPI#1 apic_id=%u vec=0x%X\n", apic_id, startup_vector);
        if (!APIC_x2apic_write_icr(apic_id, sipi_icr))
            return false;

        APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);
        kdebug_printf("[APIC] x2APIC startup SIPI#2 apic_id=%u vec=0x%X\n", apic_id, startup_vector);
        return APIC_x2apic_write_icr(apic_id, sipi_icr);
    }

    APIC_local_write(APIC_ICRH, ((uint32_t) apic_id) << 24);
    APIC_local_write(APIC_ICRL, APIC_ICR_DELIVERY_INIT | APIC_ICR_TRIGGER_LEVEL | APIC_ICR_LEVEL_ASSERT);
    if (!APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT))
        return false;

    APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);

    APIC_local_write(APIC_ICRH, ((uint32_t) apic_id) << 24);
    APIC_local_write(APIC_ICRL, APIC_ICR_DELIVERY_INIT | APIC_ICR_TRIGGER_LEVEL);
    if (!APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT))
        return false;

    APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);

    uint32_t sipi_icr = APIC_ICR_DELIVERY_STARTUP | (uint32_t) startup_vector;
    APIC_local_write(APIC_ICRH, ((uint32_t) apic_id) << 24);
    APIC_local_write(APIC_ICRL, sipi_icr);
    if (!APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT))
        return false;

    APIC_delay_loops(APIC_IPI_DELAY_SHORT_LOOPS);

    APIC_local_write(APIC_ICRH, ((uint32_t) apic_id) << 24);
    APIC_local_write(APIC_ICRL, sipi_icr);
    return APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT);
}

bool APIC_send_ipi(uint8_t apic_id, uint8_t vector)
{
    if (!APIC_runtime_state.capability.enabled)
        return false;

    if (!APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT))
        return false;

    if (APIC_runtime_state.capability.x2apic_enabled)
        return APIC_x2apic_write_icr(apic_id, (uint32_t) vector);

    APIC_local_write(APIC_ICRH, ((uint32_t) apic_id) << 24);
    APIC_local_write(APIC_ICRL, (uint32_t) vector);
    return APIC_wait_icr_idle(APIC_IPI_SPIN_TIMEOUT);
}

void APIC_send_EOI(void)
{
    if (!APIC_runtime_state.capability.enabled)
        return;

    APIC_local_write(APIC_EOI, 0);
}

bool APIC_timer_init_bsp(uint32_t hz)
{
    if (!APIC_runtime_state.capability.enabled || hz == 0)
        return false;

    __atomic_store_n(&APIC_runtime_state.timer.calibrated, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&APIC_runtime_state.timer.periodic_initial_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&APIC_runtime_state.timer.calibrated_hz, 0, __ATOMIC_RELAXED);

    bool use_hpet = HPET_is_available();
    if (!use_hpet)
        PIT_reset_ticks();

    APIC_local_write(APIC_TMRDIV, LAPIC_TIMER_DIVIDE_BY_16);
    APIC_local_write(APIC_LVT_TMR, APIC_DISABLE | (TICK_VECTOR & 0xFFU));

    const uint32_t start_count = 0xFFFFFFFFU;
    APIC_local_write(APIC_TMRINITCNT, start_count);

    uint64_t calib_ref_ticks = 0;
    if (use_hpet)
    {
        if (!HPET_wait_ms(LAPIC_TIMER_CALIBRATION_MS, &calib_ref_ticks))
        {
            APIC_local_write(APIC_LVT_TMR, APIC_DISABLE | (TICK_VECTOR & 0xFFU));
            kdebug_puts("[LAPIC] calibration timeout waiting HPET\n");
            return false;
        }
    }
    else
    {
        uint64_t spin_guard = 0;
        while (PIT_get_ticks() < LAPIC_TIMER_CALIBRATION_MS)
        {
            __asm__ __volatile__("pause");
            if (++spin_guard > 1000000000ULL)
            {
                APIC_local_write(APIC_LVT_TMR, APIC_DISABLE | (TICK_VECTOR & 0xFFU));
                kdebug_puts("[LAPIC] calibration timeout waiting PIT IRQs\n");
                return false;
            }
        }

        calib_ref_ticks = PIT_get_ticks();
    }

    uint32_t elapsed = start_count - APIC_local_read(APIC_TMRCURRCNT);
    if (elapsed == 0)
    {
        APIC_local_write(APIC_LVT_TMR, APIC_DISABLE | (TICK_VECTOR & 0xFFU));
        kdebug_puts("[LAPIC] calibration failed: zero elapsed count\n");
        return false;
    }

    uint64_t lapic_counts_per_sec = ((uint64_t) elapsed * 1000ULL) / LAPIC_TIMER_CALIBRATION_MS;
    uint32_t periodic_initial_count = (uint32_t) (lapic_counts_per_sec / hz);
    if (periodic_initial_count == 0)
        periodic_initial_count = 1;

    ISR_register_IRQ(TICK_VECTOR, APIC_timer_callback);
    APIC_local_write(APIC_TMRDIV, LAPIC_TIMER_DIVIDE_BY_16);
    APIC_local_write(APIC_LVT_TMR, (TICK_VECTOR & 0xFFU) | TMR_PERIODIC);
    APIC_local_write(APIC_TMRINITCNT, periodic_initial_count);

    __atomic_store_n(&APIC_runtime_state.timer.periodic_initial_count, periodic_initial_count, __ATOMIC_RELAXED);
    __atomic_store_n(&APIC_runtime_state.timer.calibrated_hz, hz, __ATOMIC_RELAXED);
    __atomic_store_n(&APIC_runtime_state.timer.calibrated, 1, __ATOMIC_RELEASE);

    if (!use_hpet)
        PIT_stop();

    if (use_hpet)
    {
        kdebug_printf("[LAPIC] BSP timer enabled: hz=%u init=%u calib=%u ms hpet_ticks=%llu hpet_hz=%llu cps=%llu\n",
                      hz,
                      periodic_initial_count,
                      LAPIC_TIMER_CALIBRATION_MS,
                      (unsigned long long) calib_ref_ticks,
                      (unsigned long long) HPET_get_frequency_hz(),
                      (unsigned long long) lapic_counts_per_sec);
    }
    else
    {
        kdebug_printf("[LAPIC] BSP timer enabled: hz=%u init=%u calib=%u ms pit_ticks=%llu cps=%llu\n",
                      hz,
                      periodic_initial_count,
                      LAPIC_TIMER_CALIBRATION_MS,
                      (unsigned long long) calib_ref_ticks,
                      (unsigned long long) lapic_counts_per_sec);
    }

    return true;
}

bool APIC_timer_init_ap(uint32_t hz)
{
    if (!APIC_runtime_state.capability.enabled)
        return false;

    if (__atomic_load_n(&APIC_runtime_state.timer.calibrated, __ATOMIC_ACQUIRE) == 0)
        return false;

    uint32_t periodic_initial_count = __atomic_load_n(&APIC_runtime_state.timer.periodic_initial_count, __ATOMIC_RELAXED);
    uint32_t calibrated_hz = __atomic_load_n(&APIC_runtime_state.timer.calibrated_hz, __ATOMIC_RELAXED);
    if (periodic_initial_count == 0 || calibrated_hz == 0)
        return false;

    if (hz == 0)
        hz = calibrated_hz;
    else if (hz != calibrated_hz)
        return false;

    ISR_register_IRQ(TICK_VECTOR, APIC_timer_callback);
    APIC_local_write(APIC_TMRDIV, LAPIC_TIMER_DIVIDE_BY_16);
    APIC_local_write(APIC_LVT_TMR, (TICK_VECTOR & 0xFFU) | TMR_PERIODIC);
    APIC_local_write(APIC_TMRINITCNT, periodic_initial_count);

    kdebug_printf("[LAPIC] AP timer enabled: apic_id=%u hz=%u init=%u\n",
                  APIC_get_current_lapic_id(),
                  hz,
                  periodic_initial_count);
    return true;
}

static void APIC_timer_callback(interrupt_frame_t* frame)
{
    if (APIC_get_current_lapic_id() == APIC_get_bsp_lapic_id())
        TTY_on_timer_tick();

    task_scheduler_on_tick();
    uint32_t cpu_slot = (uint32_t) APIC_get_current_lapic_id() & 0xFFU;
    (void) Syscall_handle_timer_preempt(frame, cpu_slot);
    APIC_send_EOI();
    task_irq_exit();
}

static bool APIC_wait_icr_idle(uint32_t spin_limit)
{
    for (uint32_t spin = 0; spin < spin_limit; spin++)
    {
        if ((APIC_local_read(APIC_ICRL) & APIC_ICR_DELIVERY_STATUS) == 0)
            return true;
        __asm__ __volatile__("pause");
    }

    kdebug_puts("[APIC] ICR delivery timeout\n");
    return false;
}

static void APIC_delay_loops(uint32_t loops)
{
    for (uint32_t i = 0; i < loops; i++)
        __asm__ __volatile__("pause");
}
