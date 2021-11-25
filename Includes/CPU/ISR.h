#ifndef _ISR_H
#define _ISR_H

#include <Device/TTY.h>

#include <stdint.h>
#include <stdlib.h>

typedef struct interrupt_frame
{
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, userrsp, ss;
} interrupt_frame_t;

void ISR_exception_handler(interrupt_frame_t* frame);

#endif