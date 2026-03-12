#ifndef _APIC_PRIVATE_H
#define _APIC_PRIVATE_H

#include <CPU/APIC.h>
#include <CPU/ISR.h>

static void APIC_timer_callback(interrupt_frame_t* frame);
static bool APIC_wait_icr_idle(uint32_t spin_limit);
static void APIC_delay_loops(uint32_t loops);

#endif
