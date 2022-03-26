#include <Device/PIT.h>

#include <stdio.h>

static uint32_t ticks = 0;

void PIT_init(void)
{
    PIT_phase(1); // Fire the PIT every 1ms.
    ISR_register_IRQ(IRQ0, PIT_callback);
}

void PIT_phase(uint16_t frequency)
{
    uint16_t divisor = PIT_BASE_FREQUENCY / frequency;
    
    IO_outb(PIT_COMMAND, PIT_SQUARE_MODE | PIT_ACCESS_HLB_MODE);
    IO_outb(PIT_CHANNEL0_DATA, divisor & 0XFF);
    IO_outb(PIT_CHANNEL0_DATA, divisor >> 8);
}

void PIT_sleep_ms(uint32_t ms)
{
    ticks = 0;

    printf(""); // TEMP: needed to work (tempo).

    while (ticks <= ms)
        __asm__ ("nop");
}

static void PIT_callback(interrupt_frame_t* frame)
{
    ++ticks;
}