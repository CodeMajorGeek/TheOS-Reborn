#include <CPU/APIC.h>

#include <CPU/ISR.h>
#include <Device/PIT.h>
#include <Memory/VMM.h>
#include <Memory/PMM.h>
#include <CPU/IO.h>
#include <Debug/KDebug.h>

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

static uint8_t APIC_lapic_IDs[256] = { 0 }; // CPU core Local APIC IDs.
static uint8_t APIC_num_core = 0;           // Number of cores detected.

static APIC_IO_t APIC_IOs[256] = { 0 };     // IOAPIC descriptors.
static uint8_t APIC_IO_num = 0;             // Number of IOAPICs detected. 

static uint64_t APIC_local_ptr = 0;         // Pointer to the Local APIC MMIO registers.
static uint8_t APIC_bsp_lapic_id = 0;       // BSP LAPIC ID (route destination for IOAPIC IRQs).

static bool APIC_enabled = false;
static bool APIC_supported = false;

static uint32_t APIC_IRQ_overrides[256];
static uint16_t APIC_GSI_flags[256];
static bool APIC_GSI_flags_valid[256];
static bool APIC_GSI_is_isa[256];

#define LAPIC_TIMER_DIVIDE_BY_16      0x3U
#define LAPIC_TIMER_CALIBRATION_MS    50U

static void APIC_timer_callback(interrupt_frame_t* frame);

bool APIC_check(void)
{
    uint32_t eax, edx;
    cpuid(1, &eax, &edx);

    APIC_supported = (edx & CPUID_FEAT_EDX_APIC) != 0;
    APIC_enabled = false;

    return APIC_supported;
}

void APIC_enable(void)
{
    if (!APIC_supported)
        return;

    if (APIC_local_ptr == 0)
        APIC_local_ptr = APIC_get_base();
    if (APIC_local_ptr == 0)
        return;

    APIC_disable_PIC_mode();
    APIC_set_base(APIC_local_ptr); // Hardware enable the local APIC if it wasn't enabled yet.

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

    // Use a deterministic spurious vector (0xFF). We have a stub for all 256 vectors.
    uint32_t spurious = APIC_local_read(APIC_SPURIOUS);
    spurious &= ~0xFFU;
    spurious |= 0xFFU;
    spurious |= APIC_SW_ENABLE;
    APIC_local_write(APIC_SPURIOUS, spurious);
    APIC_bsp_lapic_id = (uint8_t) (APIC_local_read(APIC_APICID) >> 24);
    APIC_enabled = true;
}

void APIC_detect_cores(APIC_MADT_t* madt)
{
    if (madt == NULL)
        return;

    kdebug_printf("[APIC] MADT=%p len=%u\n", madt, madt->SDT_header.length);

    const size_t irq_override_count = sizeof(APIC_IRQ_overrides) / sizeof(APIC_IRQ_overrides[0]);
    for (size_t i = 0; i < irq_override_count; i++)
    {
        APIC_IRQ_overrides[i] = i;
        APIC_GSI_flags[i] = 0;
        APIC_GSI_flags_valid[i] = false;
        APIC_GSI_is_isa[i] = (i < 16);
    }

    APIC_num_core = 0;
    APIC_IO_num = 0;

    APIC_local_ptr = madt->lapic_ptr;
    if (APIC_local_ptr == 0)
        APIC_local_ptr = APIC_get_base();
    if (APIC_local_ptr != 0)
    {
        uintptr_t lapic_page = (uintptr_t) APIC_local_ptr & 0xFFFFFFFFFFFFF000ULL;
        VMM_map_page(lapic_page, lapic_page);
        APIC_bsp_lapic_id = (uint8_t) (APIC_local_read(APIC_APICID) >> 24);
        kdebug_printf("[APIC] LAPIC base=0x%llX\n", APIC_local_ptr);
    }

    size_t madt_size = madt->SDT_header.length;
    uintptr_t madt_end = (uintptr_t) madt + madt_size;
    
    uintptr_t current_ptr = (uintptr_t) madt->records;
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
                if (length >= 8 && APIC_num_core < sizeof(APIC_lapic_IDs) &&
                    (*((uint32_t*) (current_ptr + 4)) & 1))
                    APIC_lapic_IDs[APIC_num_core++] = *(uint8_t*) (current_ptr + 3);
                break;
            case APIC_IO_TYPE:
                if (length < 12 || APIC_IO_num >= sizeof(APIC_IOs) / sizeof(APIC_IOs[0]))
                    break;

                kdebug_puts("[APIC] io rec start\n");
                APIC_IO_t* io = &APIC_IOs[APIC_IO_num];

                io->id = *(uint8_t*) (current_ptr + 2);
                io->ptr = *(uint32_t*) (current_ptr + 4);
                kdebug_printf("[APIC] io raw id=%u ptr=0x%X\n", io->id, io->ptr);

                uintptr_t ioapic_page = (uintptr_t) io->ptr & 0xFFFFFFFFFFFFF000ULL;
                VMM_map_page(ioapic_page, ioapic_page);
                kdebug_puts("[APIC] io mapped\n");
                io->irq_base = *((uint32_t*) (current_ptr + 8));
                kdebug_printf("[APIC] io base=%u\n", io->irq_base);
                // QEMU's IOAPIC commonly exposes 24 redirection entries (0..23).
                // Avoid probing MMIO here during very early boot.
                io->irq_end = io->irq_base + 23;
                kdebug_printf("[APIC] IOAPIC id=%u ptr=0x%X base=%u end=%u\n", io->id, io->ptr, io->irq_base, io->irq_end);

                APIC_IO_num++;
                break;
            case APIC_IO_INT_SOURCE_OVERRIDE_TYPE:
                if (length >= 10)
                {
                    uint8_t bus = *(uint8_t*) (current_ptr + 2);
                    uint8_t source_irq = *(uint8_t*) (current_ptr + 3);
                    uint32_t gsi = *(uint32_t*) (current_ptr + 4);
                    uint16_t flags = *(uint16_t*) (current_ptr + 8);

                    if ((size_t) source_irq < irq_override_count)
                        APIC_IRQ_overrides[source_irq] = gsi;

                    if ((size_t) gsi < irq_override_count)
                    {
                        APIC_GSI_flags[gsi] = flags;
                        APIC_GSI_flags_valid[gsi] = true;
                        APIC_GSI_is_isa[gsi] = (bus == 0);
                    }

                    kdebug_printf("[APIC] override bus=%u irq=%u=>gsi=%u flags=0x%X\n", bus, source_irq, gsi, flags);
                }
                break;
            case APIC_LOCAL_ADDR_OVERRIDE_TYPE:
                if (length >= 12)
                {
                    APIC_local_ptr = *(uint64_t*) (current_ptr + 4);
                    uintptr_t lapic_page = (uintptr_t) APIC_local_ptr & 0xFFFFFFFFFFFFF000ULL;
                    VMM_map_page(lapic_page, lapic_page);
                    kdebug_printf("[APIC] LAPIC override=0x%llX\n", APIC_local_ptr);
                }
                break;
            default:
                { }
        }

        current_ptr += length;
    }

    if (APIC_num_core > 0 && APIC_bsp_lapic_id == 0)
        APIC_bsp_lapic_id = APIC_lapic_IDs[0];
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
    kdebug_printf("[APIC] detect done cores=%u ioapic=%u lapic=0x%llX\n", APIC_num_core, APIC_IO_num, APIC_local_ptr);

    printf("Found %d cores, LAPIC 0x%llX, Processor IDs:", APIC_num_core, (unsigned long long) APIC_local_ptr);
    for(int i = 0; i < APIC_num_core; i++)
        printf(" %d", APIC_lapic_IDs[i]);
    printf("\n");

    printf("Found %d IOAPIC:\n", APIC_IO_num);
    for(int i = 0; i < APIC_IO_num; i++)
        printf("\tID: %d, Ptr: 0x%X, IRQ base: %d, IRQ end: %d !\n", APIC_IOs[i].id, APIC_IOs[i].ptr, APIC_IOs[i].irq_base, APIC_IOs[i].irq_end);

    printf("Redirection entries found:\n");
    const size_t irq_override_count = sizeof(APIC_IRQ_overrides) / sizeof(APIC_IRQ_overrides[0]);
    for (size_t i = 0; i < irq_override_count; i++)
        if (i != APIC_IRQ_overrides[i]) 
            printf("\t %d => %d\n", (unsigned) i, APIC_IRQ_overrides[i]);
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
    if (APIC_local_ptr == 0)
        return 0;

    return *((volatile uint32_t*) ((volatile uintptr_t) APIC_local_ptr + offset));
}

void APIC_local_write(uint32_t offset, uint32_t value)
{
    if (APIC_local_ptr == 0)
        return;

    *((volatile uint32_t*) ((volatile uintptr_t) APIC_local_ptr + offset)) = value;
}

uint32_t APIC_IO_read(uint8_t index, uint32_t reg)
{
   if (index >= APIC_IO_num || APIC_IOs[index].ptr == 0)
      return 0xFFFFFFFF;

   uint32_t volatile* ioapic = (uint32_t volatile*) ((uintptr_t) APIC_IOs[index].ptr);
   ioapic[0] = (reg & 0xff);
   return ioapic[4];
}
 
void APIC_IO_write(uint8_t index, uint32_t reg, uint32_t value)
{
   if (index >= APIC_IO_num || APIC_IOs[index].ptr == 0)
      return;

   uint32_t volatile* ioapic = (uint32_t volatile*) ((uintptr_t) APIC_IOs[index].ptr);
   ioapic[0] = (reg & 0xff);
   ioapic[4] = value;
}

bool APIC_is_enabled(void)
{
    return APIC_enabled;
}

uint8_t APIC_get_current_lapic_id(void)
{
    if (APIC_local_ptr == 0)
        return 0;

    return (uint8_t) (APIC_local_read(APIC_APICID) >> 24);
}

void APIC_register_IRQ_vector(int vec, int irq, bool disable)
{
    const size_t irq_override_count = sizeof(APIC_IRQ_overrides) / sizeof(APIC_IRQ_overrides[0]);
    if (irq < 0 || (size_t) irq >= irq_override_count)
        return;

    APIC_register_GSI_vector(vec, APIC_IRQ_overrides[irq], disable);
}

void APIC_register_GSI_vector(int vec, uint32_t gsi, bool disable)
{
    if (APIC_IO_num == 0)
        return;

    uint32_t real_IRQ = gsi;
    uint8_t IOAPIC_idx = 0;
    bool found = false;
    for (IOAPIC_idx = 0; IOAPIC_idx < APIC_IO_num; IOAPIC_idx++)
        if (real_IRQ >= APIC_IOs[IOAPIC_idx].irq_base && real_IRQ <= APIC_IOs[IOAPIC_idx].irq_end)
        {
            real_IRQ -= APIC_IOs[IOAPIC_idx].irq_base;
            found = true;
            break;
        }
    if (!found)
        return;

    uint32_t IO_lo = vec & 0xFFU;
    uint8_t lapic_id = APIC_bsp_lapic_id;
    if (lapic_id == 0 && APIC_num_core > 0)
        lapic_id = APIC_lapic_IDs[0];
    uint32_t IO_hi = ((uint32_t) lapic_id) << 24;

    bool active_low = false;
    bool level_triggered = false;
    uint16_t flags = 0;
    uint16_t polarity = APIC_MADT_INTI_POLARITY_CONFORMS;
    uint16_t trigger = APIC_MADT_INTI_TRIGGER_CONFORMS;
    bool is_isa = false;
    const size_t gsi_flag_count = sizeof(APIC_GSI_flags) / sizeof(APIC_GSI_flags[0]);

    if ((size_t) gsi < gsi_flag_count)
        is_isa = APIC_GSI_is_isa[gsi];

    if ((size_t) gsi < gsi_flag_count && APIC_GSI_flags_valid[gsi])
    {
        flags = APIC_GSI_flags[gsi];
        polarity = (uint16_t) (flags & APIC_MADT_INTI_POLARITY_MASK);
        trigger = (uint16_t) ((flags & APIC_MADT_INTI_TRIGGER_MASK) >> APIC_MADT_INTI_TRIGGER_SHIFT);
    }

    // ACPI "conforms" follows the source bus. For ISA defaults: active high + edge.
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

    uint32_t reg = 0x10 + (real_IRQ * 2);
    APIC_IO_write(IOAPIC_idx, reg, IO_lo);
    APIC_IO_write(IOAPIC_idx, reg + 1, IO_hi);
    kdebug_printf("[APIC] route GSI %u -> vec 0x%X (LAPIC %u) flags=0x%X pol=%s trig=%s%s\n",
                  gsi, vec, lapic_id, flags,
                  active_low ? "low" : "high",
                  level_triggered ? "level" : "edge",
                  disable ? " [masked]" : "");
}

void APIC_send_EOI(void)
{
    if (!APIC_enabled)
        return;

    APIC_local_write(APIC_EOI, 0);
}

bool APIC_timer_init_bsp(uint32_t hz)
{
    if (!APIC_enabled || hz == 0)
        return false;

    PIT_reset_ticks();

    APIC_local_write(APIC_TMRDIV, LAPIC_TIMER_DIVIDE_BY_16);
    APIC_local_write(APIC_LVT_TMR, APIC_DISABLE | (TICK_VECTOR & 0xFFU));

    const uint32_t start_count = 0xFFFFFFFFU;
    APIC_local_write(APIC_TMRINITCNT, start_count);

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

    PIT_stop();

    kdebug_printf("[LAPIC] BSP timer enabled: hz=%u init=%u calib=%u ms pit_ticks=%llu cps=%llu\n",
                  hz,
                  periodic_initial_count,
                  LAPIC_TIMER_CALIBRATION_MS,
                  (unsigned long long) PIT_get_ticks(),
                  (unsigned long long) lapic_counts_per_sec);

    return true;
}

static void APIC_timer_callback(interrupt_frame_t* frame)
{
    (void) frame;
    APIC_send_EOI();
}
