#include <Device/PIT.h>

#include <stdio.h>

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

static void PIT_callback(interrupt_frame_t* frame)
{
    // puts("IRQ0 fired !\n");
}