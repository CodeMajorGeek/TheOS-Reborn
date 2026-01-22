#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>
#include <stdint.h>

#define SYSCALL_INT 0x80

#define SYSCALL_FMASK_TF_BIT        (1ULL << 8)
#define SYSCALL_FMASK_DF_BIT        (1ULL << 10)

extern void enable_syscall_ext(void);
extern void syscall_handler_stub(void);

void Syscall_init(void);

void Syscall_interupt_handler(uint64_t syscall_num, interrupt_frame_t* frame);

#endif