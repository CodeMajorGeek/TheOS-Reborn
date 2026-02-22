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
    SYS_FS_CREATE = 3,
    SYS_SLEEP_MS = 4,
    SYS_TICK_GET = 5,
    SYS_CPU_INFO_GET = 6,
    SYS_SCHED_INFO_GET = 7,
    SYS_AHCI_IRQ_INFO_GET = 8,
    SYS_RCU_SYNC = 9,
    SYS_RCU_INFO_GET = 10,
    SYS_CONSOLE_WRITE = 11
};

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

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index);

#endif
