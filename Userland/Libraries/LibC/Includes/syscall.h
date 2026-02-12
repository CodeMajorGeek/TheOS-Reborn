#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stddef.h>
#include <stdint.h>

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
    SYS_RCU_INFO_GET = 10
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

long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6);

int fs_ls(void);
int fs_read(const char* name, void* buf, size_t buf_size, size_t* out_size);
int fs_create(const char* name, const void* data, size_t size);
int sys_sleep_ms(uint32_t ms);
uint64_t sys_tick_get(void);
int sys_cpu_info_get(syscall_cpu_info_t* out_info);
int sys_sched_info_get(syscall_sched_info_t* out_info);
int sys_ahci_irq_info_get(syscall_ahci_irq_info_t* out_info);
int sys_rcu_sync(void);
int sys_rcu_info_get(syscall_rcu_info_t* out_info);

#endif
