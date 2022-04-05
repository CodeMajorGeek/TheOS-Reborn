#ifndef _PIT_H
#define _PIT_H

#include <CPU/ISR.h>

#include <stdint.h>

#define PIT_BASE_FREQUENCY  1193180 // The 1.193182 MHz input signal.

#define PIT_CHANNEL0_DATA   0x40
#define PIT_CHANNEL1_DATA   0x41
#define PIT_CHANNEL2_DATA   0x42
#define PIT_COMMAND         0x43

#define PIT_BCD_MODE                0b00000000
#define PIT_SQUARE_MODE             0b00000110
#define PIT_RATE_MODE               0b00001100
#define PIT_HARDWARE_TRIG_MODE      0b00001010
#define PIT_SOFTWARE_TRIG_MODE      0b00001000
#define PIT_HARDWARE_RETRIG_MODE    0b00000010
#define PIT_INTERRUPT_TERM_MODE     0

#define PIT_ACCESS_LB_MODE          0b00010000
#define PIT_ACCESS_HB_MODE          0b00100000
#define PIT_ACCESS_HLB_MODE         0b00110000

#define PIT_NULL_COUNT_FLAG         0b01000000
#define PIT_OUTPUT_PIN_HIGH         0b10000000        


void PIT_init(void);
void PIT_phase(uint16_t);

void PIT_sleep_ms(uint32_t);

static void PIT_callback(interrupt_frame_t*);

#endif