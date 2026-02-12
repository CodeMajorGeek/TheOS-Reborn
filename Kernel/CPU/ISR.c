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
static bool pic_irq_seen_while_apic = false;
static bool pic_quiet_reported = false;
static bool ioapic_vector_seen[MAX_IRQ_ENTRIES] = { false };
static volatile uint64_t irq_vector_count[MAX_IRQ_ENTRIES] = { 0 };
static volatile uint64_t timer_ticks = 0;
static volatile uint64_t pic_activity_count = 0;
static volatile uint64_t irq_range_seen = 0;
static volatile tick_source_t tick_source = TICK_SOURCE_PIT_IOAPIC;
static volatile uint32_t tick_hz = 1000;

#define TICK_LOG_PERIOD             100ULL
#define PIC_CHECK_PERIOD            100ULL
#define PIC_QUIET_REPORT_AFTER      500ULL

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

void ISR_set_tick_source(tick_source_t source, uint32_t hz)
{
    tick_source = source;

    if (hz != 0)
        tick_hz = hz;
}

tick_source_t ISR_get_tick_source(void)
{
    return (tick_source_t) tick_source;
}

uint32_t ISR_get_tick_hz(void)
{
    return tick_hz;
}

uint64_t ISR_get_timer_ticks(void)
{
    return timer_ticks;
}

uint64_t ISR_get_vector_count(uint8_t vector)
{
    if (vector < IRQ_VECTOR_BASE || vector > IRQ_VECTOR_END)
        return 0;

    return irq_vector_count[vector - IRQ_VECTOR_BASE];
}

uint64_t ISR_get_pic_activity_count(void)
{
    return pic_activity_count;
}

void ISR_handler(interrupt_frame_t* frame)
{
    if (frame->int_no < MAX_KNOWN_EXCEPTIONS)
    {
        kdebug_printf(
            "[ISR] Exception %llu (%s)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            exception_messages[frame->int_no],
            frame->rip,
            frame->cs,
            frame->rflags
        );
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
    }

    abort();
}


void IRQ_handler(interrupt_frame_t *frame)
{
    int vector = (int) frame->int_no;
    int irq = vector - IRQ_VECTOR_BASE;

    if (irq < 0 || irq >= MAX_IRQ_ENTRIES)
        return;

    uint64_t vector_hits = ++irq_vector_count[irq];
    ++irq_range_seen;

    if ((uint8_t) vector == TICK_VECTOR)
    {
        timer_ticks = vector_hits;

        if ((vector_hits % TICK_LOG_PERIOD) == 0)
        {
            uint32_t cpu_id = APIC_is_enabled() ? (uint32_t) APIC_get_current_lapic_id() : 0;
            kdebug_printf("[TICK] src=%s vec=0x%X cpu=%u count=%llu\n",
                          ISR_tick_source_name((tick_source_t) tick_source),
                          (unsigned) vector,
                          cpu_id,
                          (unsigned long long) vector_hits);
        }
    }

    IRQ_t handler = IRQ_handlers[irq];
    if (handler)
        handler(frame);

    if (APIC_is_enabled())
    {
        if ((uint8_t) vector != TICK_VECTOR && !ioapic_vector_seen[irq])
        {
            ioapic_vector_seen[irq] = true;
            kdebug_printf("[IRQ] IOAPIC delivery confirmed vec=0x%X count=%llu\n",
                          (unsigned) vector,
                          (unsigned long long) vector_hits);
        }

        if ((irq_range_seen % PIC_CHECK_PERIOD) == 0)
        {
            uint16_t pic_isr = PIC_get_ISR();
            uint16_t pic_irr = PIC_get_IRR();
            uint16_t pic_mask = PIC_get_mask();
            uint16_t pic_unmasked_irr = pic_irr & (uint16_t) ~pic_mask;
            if ((pic_isr | pic_unmasked_irr) != 0)
            {
                ++pic_activity_count;
                if (!pic_irq_seen_while_apic)
                {
                    pic_irq_seen_while_apic = true;
                    kdebug_printf("[IRQ] WARNING: PIC active while APIC enabled (ISR=0x%X IRR=0x%X IMR=0x%X vec=0x%X)\n",
                                  pic_isr, pic_irr, pic_mask, (unsigned) vector);
                }
            }
        }

        if (!pic_quiet_reported && irq_range_seen >= PIC_QUIET_REPORT_AFTER)
        {
            pic_quiet_reported = true;
            if (pic_activity_count == 0)
                kdebug_printf("[IRQ] anti-PIC check: no PIC activity after %llu IRQ-range vectors\n",
                              (unsigned long long) irq_range_seen);
            else
                kdebug_printf("[IRQ] anti-PIC check: PIC activity seen %llu times after %llu IRQ-range vectors\n",
                              (unsigned long long) pic_activity_count,
                              (unsigned long long) irq_range_seen);
        }

        // Vector 0x20 acknowledges LAPIC EOI in the active tick callback (PIT during calibration or LAPIC timer).
        if ((uint8_t) vector != TICK_VECTOR)
            APIC_send_EOI();
    }
    else
        PIC_send_EOI((uint8_t) irq);
}
