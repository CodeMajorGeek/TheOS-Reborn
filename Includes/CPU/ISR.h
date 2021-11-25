#ifndef _ISR_H
#define _ISR_H

#include <Device/TTY.h>

#include <stdint.h>
#include <stdlib.h>

#define MAX_KNOWN_EXCEPTIONS    19

typedef struct interrupt_frame
{
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, userrsp, ss;
} interrupt_frame_t;

const char** exception_messages[MAX_KNOWN_EXCEPTIONS] =
{
"Divide By Zero",
"Debug",
"Not Maskable Interrupt",
"Int 3",
"INTO",
"Out of Bounds",
"Invalid Opcode",
"Coprocessor Not Acailable",
"Double Fault",
"Coprocessor Segment Overrun",
"Bad TSS",
"Stack No Present",
"Stack Fault",
"General Protection Fault",
"Page Fault",
"Reserved",
"Floating Point",
"Alignment Check",
"Machine Check"
};

void ISR_exception_handler(interrupt_frame_t frame);

#endif