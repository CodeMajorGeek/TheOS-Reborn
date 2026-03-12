#include <Device/PIT.h>
#include <Device/PIT_private.h>

#include <CPU/IO.h>
#include <CPU/APIC.h>
#include <Device/TTY.h>
#include <Debug/KDebug.h>
#include <Task/Task.h>

static PIT_runtime_state_t PIT_state;

void PIT_init(void)
{
    PIT_phase(1000); // Fire the PIT every 1ms.
    PIT_reset_ticks();
    ISR_register_IRQ(TICK_VECTOR, PIT_callback);

    if (APIC_is_enabled())
    {
        APIC_register_GSI_vector(TICK_VECTOR, 2, FALSE);
        kdebug_puts("[PIT] IOAPIC route GSI2 -> vec 0x20 (calibration path)\n");
    }
}

void PIT_stop(void)
{
    // Reprogram channel 0 in one-shot mode with max reload value to quiesce periodic IRQ generation.
    IO_outb(PIT_COMMAND, PIT_INTERRUPT_TERM_MODE | PIT_ACCESS_HLB_MODE);
    IO_outb(PIT_CHANNEL0_DATA, 0x00);
    IO_outb(PIT_CHANNEL0_DATA, 0x00);

    if (APIC_is_enabled())
    {
        APIC_register_GSI_vector(TICK_VECTOR, 2, TRUE);
        kdebug_puts("[PIT] IOAPIC route GSI2 masked\n");
    }
}

void PIT_phase(uint16_t frequency)
{
    if (frequency == 0)
        frequency = 1;

    uint16_t divisor = PIT_BASE_FREQUENCY / frequency;
    
    IO_outb(PIT_COMMAND, PIT_SQUARE_MODE | PIT_ACCESS_HLB_MODE);
    IO_outb(PIT_CHANNEL0_DATA, divisor & 0XFF);
    IO_outb(PIT_CHANNEL0_DATA, divisor >> 8);
}

void PIT_reset_ticks(void)
{
    PIT_state.ticks = 0;
}

uint64_t PIT_get_ticks(void)
{
    return PIT_state.ticks;
}

void PIT_sleep_ms(uint32_t ms)
{
    uint32_t hz = ISR_get_tick_hz();
    if (hz == 0)
        hz = 1000;

    uint64_t start = ISR_get_timer_ticks();
    uint64_t wait_ticks = ((uint64_t) ms * hz + 999ULL) / 1000ULL;
    if (wait_ticks == 0)
        wait_ticks = 1;

    while ((ISR_get_timer_ticks() - start) < wait_ticks)
        __asm__ __volatile__("pause");
}

static void PIT_callback(interrupt_frame_t* frame)
{
    (void) frame;
    ++PIT_state.ticks;
    TTY_on_timer_tick();
    task_scheduler_on_tick();

    if (APIC_is_enabled())
    {
        if (!PIT_state.irq_seen_with_apic)
        {
            PIT_state.irq_seen_with_apic = true;
            kdebug_puts("[PIT] vec 0x20 observed with APIC enabled (delivered through IOAPIC)\n");
        }
        APIC_send_EOI();
    }

    task_irq_exit();
}
