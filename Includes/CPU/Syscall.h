#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>
#include <stdbool.h>
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
    SYS_FS_CREATE = 3,
    SYS_SLEEP_MS = 4,
    SYS_TICK_GET = 5,
    SYS_CPU_INFO_GET = 6,
    SYS_SCHED_INFO_GET = 7,
    SYS_AHCI_IRQ_INFO_GET = 8,
    SYS_RCU_SYNC = 9,
    SYS_RCU_INFO_GET = 10,
    SYS_CONSOLE_WRITE = 11,
    SYS_EXIT = 12,
    SYS_FORK = 13,
    SYS_EXECVE = 14,
    SYS_YIELD = 15,
    SYS_MAP = 16,
    SYS_UNMAP = 17,
    SYS_MPROTECT = 18,
    SYS_FS_WRITE = 19,
    SYS_OPEN = 20,
    SYS_CLOSE = 21,
    SYS_FS_SEEK = 22,
    SYS_KBD_GET_SCANCODE = 23,
    SYS_FS_ISDIR = 24,
    SYS_FS_MKDIR = 25,
    SYS_WAITPID = 26
};

#define SYS_PROT_READ    (1ULL << 0)
#define SYS_PROT_WRITE   (1ULL << 1)
#define SYS_PROT_EXEC    (1ULL << 2)

#define SYS_OPEN_READ    (1ULL << 0)
#define SYS_OPEN_WRITE   (1ULL << 1)
#define SYS_OPEN_CREATE  (1ULL << 2)
#define SYS_OPEN_TRUNC   (1ULL << 3)

#define SYS_SEEK_SET     0
#define SYS_SEEK_CUR     1
#define SYS_SEEK_END     2

#define SYS_SIGILL       4
#define SYS_SIGTRAP      5
#define SYS_SIGFPE       8
#define SYS_SIGSEGV      11
#define SYS_SIGFAULT     128

typedef struct syscall_cpu_info
{
    uint32_t cpu_index;
    uint32_t apic_id;
    uint32_t online_cpus;
    uint32_t tick_hz;
    uint64_t ticks;
} syscall_cpu_info_t;

typedef struct syscall_sched_info
{
    uint32_t current_cpu;
    uint32_t preempt_count;
    uint32_t local_rq_depth;
    uint32_t total_rq_depth;
} syscall_sched_info_t;

typedef struct syscall_ahci_irq_info
{
    uint32_t mode;
    uint32_t reserved;
    uint64_t count;
} syscall_ahci_irq_info_t;

typedef struct syscall_rcu_info
{
    uint64_t gp_seq;
    uint64_t gp_target;
    uint64_t callbacks_pending;
    uint32_t local_read_depth;
    uint32_t local_preempt_count;
} syscall_rcu_info_t;

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

#endif
