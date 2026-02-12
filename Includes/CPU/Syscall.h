#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>
#include <stdint.h>

#define SYSCALL_INT 0x80

#define SYSCALL_FMASK_TF_BIT        (1ULL << 8)
#define SYSCALL_FMASK_DF_BIT        (1ULL << 10)

extern void enable_syscall_ext(void);
extern void syscall_handler_stub(void);

enum
{
    SYS_FS_LS = 1,
    SYS_FS_READ = 2,
    SYS_FS_CREATE = 3
};

typedef struct syscall_frame
{
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
} syscall_frame_t;

void Syscall_init(void);

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame);

#endif
