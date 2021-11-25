#ifndef _ISR_H
#define _ISR_H

#include <stdint.h>

typedef struct interrupt_frame
{
    uint16_t ip;

} interrupt_frame_t;

__attribute__((interrupt)) void interupt_handler(interrupt_frame_t* frame);

#endif