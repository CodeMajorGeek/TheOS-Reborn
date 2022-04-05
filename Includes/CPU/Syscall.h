#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>

#define SYSCALL_INT 0x80

extern void enable_syscall_ext(void);
extern void syscall_handler_stub(void);

void Syscall_init(void);

void Syscall_interupt_handler(interrupt_frame_t* frame);
void syscall_handler(void);

#endif