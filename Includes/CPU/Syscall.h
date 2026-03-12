#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>
#include <UAPI/Syscall.h>
#include <stdbool.h>
#include <stdint.h>

#define SYSCALL_INT 0x80

#define SYSCALL_FMASK_TF_BIT        (1ULL << 8)
#define SYSCALL_FMASK_DF_BIT        (1ULL << 10)

extern void enable_syscall_ext(void);
extern void syscall_handler_stub(void);

typedef struct syscall_frame
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t reserved0;
} syscall_frame_t;

void Syscall_init(void);

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index);
uint64_t Syscall_post_handler(uint64_t syscall_ret, syscall_frame_t* frame, uint32_t cpu_index);
bool Syscall_handle_user_exception(interrupt_frame_t* frame, uintptr_t fault_addr);
void Syscall_on_timer_tick(uint32_t cpu_index);
bool Syscall_handle_timer_preempt(interrupt_frame_t* frame, uint32_t cpu_index);

#endif
