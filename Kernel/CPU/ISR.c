#include <CPU/ISR.h>

#include <Device/PIC.h>
#include <CPU/Syscall.h>
#include <CPU/APIC.h>
#include <CPU/GDT.h>
#include <Debug/KDebug.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

IRQ_t IRQ_handlers[MAX_IRQ_ENTRIES];
static IRQ_t ISR_vector_handlers[IDT_MAX_VECTORS] = { 0 };
static uint8_t pic_irq_seen_while_apic = 0;
static uint8_t pic_quiet_reported = 0;
static uint8_t ioapic_vector_seen[MAX_IRQ_ENTRIES] = { 0 };
static uint64_t irq_vector_count[MAX_IRQ_ENTRIES] = { 0 };
static uint64_t timer_ticks = 0;
static uint64_t tick_cpu_hits[256] = { 0 };
static uint64_t pic_activity_count = 0;
static uint64_t irq_range_seen = 0;
static tick_source_t tick_source = TICK_SOURCE_PIT_IOAPIC;
static uint32_t tick_hz = 1000;

static const char* ISR_tick_source_name(tick_source_t source)
{
    switch (source)
    {
        case TICK_SOURCE_PIT_IOAPIC:
            return "PIT_IOAPIC";
        case TICK_SOURCE_LAPIC_TIMER:
            return "LAPIC";
        default:
            return "UNKNOWN";
    }
}

void ISR_register_IRQ(int vector, IRQ_t irq)
{
    int irq_index = vector - ISR_COUNT_BEFORE_IRQ;

    if (irq_index < 0 || irq_index >= MAX_IRQ_ENTRIES)
        return;

    IRQ_handlers[irq_index] = irq;

    if (APIC_is_enabled())
    {
        // Keep vector 0x20 for the scheduler tick source (PIT during calibration, then LAPIC timer).
        if (vector != TICK_VECTOR)
            APIC_register_IRQ_vector(vector, irq_index, FALSE);
    }
}

void ISR_register_vector(uint8_t vector, IRQ_t handler)
{
    ISR_vector_handlers[vector] = handler;
}

void ISR_set_tick_source(tick_source_t source, uint32_t hz)
{
    __atomic_store_n(&tick_source, source, __ATOMIC_RELEASE);

    if (hz != 0)
        __atomic_store_n(&tick_hz, hz, __ATOMIC_RELEASE);
}

tick_source_t ISR_get_tick_source(void)
{
    return __atomic_load_n(&tick_source, __ATOMIC_ACQUIRE);
}

uint32_t ISR_get_tick_hz(void)
{
    return __atomic_load_n(&tick_hz, __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_timer_ticks(void)
{
    return __atomic_load_n(&timer_ticks, __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_vector_count(uint8_t vector)
{
    if (vector < IRQ_VECTOR_BASE || vector > IRQ_VECTOR_END)
        return 0;

    return __atomic_load_n(&irq_vector_count[vector - IRQ_VECTOR_BASE], __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_pic_activity_count(void)
{
    return __atomic_load_n(&pic_activity_count, __ATOMIC_ACQUIRE);
}

void ISR_handler(interrupt_frame_t* frame)
{
    if (frame->int_no < IDT_MAX_VECTORS)
    {
        IRQ_t handler = ISR_vector_handlers[frame->int_no];
        if (handler)
        {
            handler(frame);
            return;
        }
    }

    if (frame->int_no < MAX_KNOWN_EXCEPTIONS)
    {
        if (frame->int_no == 14)
        {
            uintptr_t fault_addr = 0;
            __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_addr));
            kdebug_printf(
                "[ISR] Exception %llu (%s)\n"
                "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX  ERR=0x%llX  CR2=0x%llX\n",
                frame->int_no,
                exception_messages[frame->int_no],
                frame->rip,
                frame->cs,
                frame->rflags,
                frame->err_code,
                (unsigned long long) fault_addr
            );
            abort();
        }

        kdebug_printf(
            "[ISR] Exception %llu (%s)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            exception_messages[frame->int_no],
            frame->rip,
            frame->cs,
            frame->rflags
        );

        abort();
    }

    if (frame->int_no >= ISR_COUNT_BEFORE_IRQ)
    {
        kdebug_printf(
            "[ISR] Unhandled vector %llu\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            frame->rip,
            frame->cs,
            frame->rflags
        );

        if (APIC_is_enabled())
            APIC_send_EOI();

        return;
    }
    else
    {
        kdebug_printf(
            "[ISR] Exception %llu (Unknown)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            frame->rip,
            frame->cs,
            frame->rflags
        );

        abort();
    }
}


void IRQ_handler(interrupt_frame_t *frame)
{
    int vector = (int) frame->int_no;
    int irq = vector - IRQ_VECTOR_BASE;

    if (irq < 0 || irq >= MAX_IRQ_ENTRIES)
        return;

    uint64_t vector_hits = __atomic_add_fetch(&irq_vector_count[irq], 1, __ATOMIC_RELAXED);
    uint64_t range_hits = __atomic_add_fetch(&irq_range_seen, 1, __ATOMIC_RELAXED);

    if ((uint8_t) vector == TICK_VECTOR)
    {
        uint32_t cpu_id = APIC_is_enabled() ? (uint32_t) APIC_get_current_lapic_id() : 0;
        uint32_t cpu_slot = cpu_id & 0xFFU;
        uint64_t cpu_ticks = __atomic_add_fetch(&tick_cpu_hits[cpu_slot], 1, __ATOMIC_RELAXED);
        uint64_t total_ticks = __atomic_add_fetch(&timer_ticks, 1, __ATOMIC_RELAXED);
        tick_source_t source = __atomic_load_n(&tick_source, __ATOMIC_RELAXED);

        if ((cpu_ticks % TICK_LOG_PERIOD) == 0)
        {
            kdebug_printf("[TICK] src=%s vec=0x%X cpu=%u cpu_count=%llu total=%llu\n",
                          ISR_tick_source_name(source),
                          (unsigned) vector,
                          cpu_slot,
                          (unsigned long long) cpu_ticks,
                          (unsigned long long) total_ticks);
        }
    }

    IRQ_t handler = IRQ_handlers[irq];
    if (handler)
        handler(frame);

    if (APIC_is_enabled())
    {
        if ((uint8_t) vector != TICK_VECTOR &&
            __atomic_exchange_n(&ioapic_vector_seen[irq], 1, __ATOMIC_ACQ_REL) == 0)
        {
            kdebug_printf("[IRQ] IOAPIC delivery confirmed vec=0x%X count=%llu\n",
                          (unsigned) vector,
                          (unsigned long long) vector_hits);
        }

        if ((range_hits % PIC_CHECK_PERIOD) == 0)
        {
            uint16_t pic_isr = PIC_get_ISR();
            uint16_t pic_irr = PIC_get_IRR();
            uint16_t pic_mask = PIC_get_mask();
            uint16_t pic_unmasked_irr = pic_irr & (uint16_t) ~pic_mask;
            if ((pic_isr | pic_unmasked_irr) != 0)
            {
                __atomic_fetch_add(&pic_activity_count, 1, __ATOMIC_RELAXED);
                if (__atomic_exchange_n(&pic_irq_seen_while_apic, 1, __ATOMIC_ACQ_REL) == 0)
                {
                    kdebug_printf("[IRQ] WARNING: PIC active while APIC enabled (ISR=0x%X IRR=0x%X IMR=0x%X vec=0x%X)\n",
                                  pic_isr, pic_irr, pic_mask, (unsigned) vector);
                }
            }
        }

        if (__atomic_load_n(&pic_quiet_reported, __ATOMIC_ACQUIRE) == 0 &&
            range_hits >= PIC_QUIET_REPORT_AFTER &&
            __atomic_exchange_n(&pic_quiet_reported, 1, __ATOMIC_ACQ_REL) == 0)
        {
            uint64_t pic_hits = __atomic_load_n(&pic_activity_count, __ATOMIC_RELAXED);
            if (pic_hits == 0)
                kdebug_printf("[IRQ] anti-PIC check: no PIC activity after %llu IRQ-range vectors\n",
                              (unsigned long long) range_hits);
            else
                kdebug_printf("[IRQ] anti-PIC check: PIC activity seen %llu times after %llu IRQ-range vectors\n",
                              (unsigned long long) pic_hits,
                              (unsigned long long) range_hits);
        }

        // Vector 0x20 acknowledges LAPIC EOI in the active tick callback (PIT during calibration or LAPIC timer).
        if ((uint8_t) vector != TICK_VECTOR)
            APIC_send_EOI();
    }
    else
        PIC_send_EOI((uint8_t) irq);
}
