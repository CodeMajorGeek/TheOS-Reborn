#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>

#define SYSCALL_INT 0x80

extern void enable_syscall_ext(void);

void Syscall_init(void);

void Syscall_interupt_handler(interrupt_frame_t* frame);

#endif