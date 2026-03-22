#include <CPU/ISR.h>

#include <Device/PIC.h>
#include <Device/COM.h>
#include <CPU/Syscall.h>
#include <CPU/APIC.h>
#include <CPU/GDT.h>
#include <Debug/KDebug.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static ISR_runtime_state_t ISR_state = {
    .tick_source = TICK_SOURCE_PIT_IOAPIC,
    .tick_hz = 1000
};

static void ISR_serial_puts_unsafe(const char* str)
{
    if (!str)
        return;
    COM_puts(COM1, str);
}

static void ISR_serial_put_hex64_unsafe(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char buffer[16];
    for (int i = 15; i >= 0; i--)
    {
        buffer[i] = digits[value & 0xFU];
        value >>= 4;
    }
    COM_write(COM1, buffer, sizeof(buffer));
}

static void ISR_serial_put_uint_unsafe(uint64_t value)
{
    char buffer[21];
    size_t index = 0;
    if (value == 0)
    {
        COM_putc(COM1, '0');
        return;
    }

    while (value != 0 && index < sizeof(buffer))
    {
        buffer[index++] = (char) ('0' + (value % 10U));
        value /= 10U;
    }

    while (index > 0)
    {
        index--;
        COM_putc(COM1, buffer[index]);
    }
}

static void ISR_serial_dump_exception_unsafe(const interrupt_frame_t* frame, uintptr_t fault_addr)
{
    if (!frame)
        return;

    ISR_serial_puts_unsafe("[ISR-RAW] Exception ");
    ISR_serial_put_uint_unsafe(frame->int_no);
    ISR_serial_puts_unsafe(" RIP=0x");
    ISR_serial_put_hex64_unsafe(frame->rip);
    ISR_serial_puts_unsafe(" CS=0x");
    ISR_serial_put_hex64_unsafe(frame->cs);
    ISR_serial_puts_unsafe(" RFLAGS=0x");
    ISR_serial_put_hex64_unsafe(frame->rflags);
    ISR_serial_puts_unsafe(" ERR=0x");
    ISR_serial_put_hex64_unsafe(frame->err_code);
    ISR_serial_puts_unsafe(" CR2=0x");
    ISR_serial_put_hex64_unsafe((uint64_t) fault_addr);
    ISR_serial_puts_unsafe("\n");
}

static const char* ISR_exception_messages[MAX_KNOWN_EXCEPTIONS] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating Point",
    "Alignment Check",
    "Machine Check"
};

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

    ISR_state.irq_handlers[irq_index] = irq;

    if (APIC_is_enabled())
    {
        // Keep vector 0x20 for the scheduler tick source (PIT during calibration, then LAPIC timer).
        if (vector != TICK_VECTOR)
            APIC_register_IRQ_vector(vector, irq_index, FALSE);
    }
}

void ISR_register_vector(uint8_t vector, IRQ_t handler)
{
    ISR_state.vector_handlers[vector] = handler;
}

void ISR_set_tick_source(tick_source_t source, uint32_t hz)
{
    __atomic_store_n(&ISR_state.tick_source, source, __ATOMIC_RELEASE);

    if (hz != 0)
        __atomic_store_n(&ISR_state.tick_hz, hz, __ATOMIC_RELEASE);
}

tick_source_t ISR_get_tick_source(void)
{
    return __atomic_load_n(&ISR_state.tick_source, __ATOMIC_ACQUIRE);
}

uint32_t ISR_get_tick_hz(void)
{
    return __atomic_load_n(&ISR_state.tick_hz, __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_timer_ticks(void)
{
    return __atomic_load_n(&ISR_state.timer_ticks, __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_vector_count(uint8_t vector)
{
    if (vector < IRQ_VECTOR_BASE || vector > IRQ_VECTOR_END)
        return 0;

    return __atomic_load_n(&ISR_state.irq_vector_count[vector - IRQ_VECTOR_BASE], __ATOMIC_ACQUIRE);
}

uint64_t ISR_get_pic_activity_count(void)
{
    return __atomic_load_n(&ISR_state.pic_activity_count, __ATOMIC_ACQUIRE);
}

void ISR_handler(interrupt_frame_t* frame)
{
    if (frame->int_no < IDT_MAX_VECTORS)
    {
        IRQ_t handler = ISR_state.vector_handlers[frame->int_no];
        if (handler)
        {
            handler(frame);
            return;
        }
    }

    if (frame->int_no < MAX_KNOWN_EXCEPTIONS)
    {
        bool from_user = ((frame->cs & 0x3ULL) == 0x3ULL);
        uintptr_t fault_addr = 0;
        if (frame->int_no == 14)
            __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_addr));

        if (from_user && Syscall_handle_user_exception(frame, fault_addr))
            return;

        if (frame->int_no == 14)
        {
            ISR_serial_dump_exception_unsafe(frame, fault_addr);
            kdebug_printf(
                "[ISR] Exception %llu (%s)\n"
                "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX  ERR=0x%llX  CR2=0x%llX\n",
                frame->int_no,
                ISR_exception_messages[frame->int_no],
                frame->rip,
                frame->cs,
                frame->rflags,
                frame->err_code,
                (unsigned long long) fault_addr
            );
            abort();
        }

        ISR_serial_dump_exception_unsafe(frame, fault_addr);
        kdebug_printf(
            "[ISR] Exception %llu (%s)\n"
            "      RIP=0x%llX  CS=0x%llX  RFLAGS=0x%llX\n",
            frame->int_no,
            ISR_exception_messages[frame->int_no],
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
        ISR_serial_dump_exception_unsafe(frame, 0);
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
    bool is_tick_vector = ((uint8_t) vector == TICK_VECTOR);
    uint32_t tick_cpu_slot = 0;

    if (irq < 0 || irq >= MAX_IRQ_ENTRIES)
        return;

    uint64_t vector_hits = __atomic_add_fetch(&ISR_state.irq_vector_count[irq], 1, __ATOMIC_RELAXED);
    uint64_t range_hits = __atomic_add_fetch(&ISR_state.irq_range_seen, 1, __ATOMIC_RELAXED);

    if (is_tick_vector)
    {
        uint32_t cpu_id = APIC_is_enabled() ? (uint32_t) APIC_get_current_lapic_id() : 0;
        tick_cpu_slot = cpu_id & 0xFFU;
        uint64_t cpu_ticks = __atomic_add_fetch(&ISR_state.tick_cpu_hits[tick_cpu_slot], 1, __ATOMIC_RELAXED);
        tick_source_t source = __atomic_load_n(&ISR_state.tick_source, __ATOMIC_RELAXED);
        bool contributes_to_wall_clock = true;
        if (source == TICK_SOURCE_LAPIC_TIMER && APIC_is_enabled())
        {
            uint8_t bsp_lapic_id = APIC_get_bsp_lapic_id();
            if (bsp_lapic_id != 0xFFU)
                contributes_to_wall_clock = ((uint8_t) cpu_id == bsp_lapic_id);
        }

        uint64_t total_ticks;
        if (contributes_to_wall_clock)
            total_ticks = __atomic_add_fetch(&ISR_state.timer_ticks, 1, __ATOMIC_RELAXED);
        else
            total_ticks = __atomic_load_n(&ISR_state.timer_ticks, __ATOMIC_RELAXED);

        Syscall_on_timer_tick(tick_cpu_slot);

        if ((cpu_ticks % TICK_LOG_PERIOD) == 0)
        {
            kdebug_printf("[TICK] src=%s vec=0x%X cpu=%u cpu_count=%llu total=%llu\n",
                          ISR_tick_source_name(source),
                          (unsigned) vector,
                          tick_cpu_slot,
                          (unsigned long long) cpu_ticks,
                          (unsigned long long) total_ticks);
        }
    }

    IRQ_t handler = ISR_state.irq_handlers[irq];
    if (handler)
        handler(frame);

    if (APIC_is_enabled())
    {
        if ((uint8_t) vector != TICK_VECTOR &&
            __atomic_exchange_n(&ISR_state.ioapic_vector_seen[irq], 1, __ATOMIC_ACQ_REL) == 0)
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
                __atomic_fetch_add(&ISR_state.pic_activity_count, 1, __ATOMIC_RELAXED);
                if (__atomic_exchange_n(&ISR_state.pic_irq_seen_while_apic, 1, __ATOMIC_ACQ_REL) == 0)
                {
                    kdebug_printf("[IRQ] WARNING: PIC active while APIC enabled (ISR=0x%X IRR=0x%X IMR=0x%X vec=0x%X)\n",
                                  pic_isr, pic_irr, pic_mask, (unsigned) vector);
                }
            }
        }

        if (__atomic_load_n(&ISR_state.pic_quiet_reported, __ATOMIC_ACQUIRE) == 0 &&
            range_hits >= PIC_QUIET_REPORT_AFTER &&
            __atomic_exchange_n(&ISR_state.pic_quiet_reported, 1, __ATOMIC_ACQ_REL) == 0)
        {
            uint64_t pic_hits = __atomic_load_n(&ISR_state.pic_activity_count, __ATOMIC_RELAXED);
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
