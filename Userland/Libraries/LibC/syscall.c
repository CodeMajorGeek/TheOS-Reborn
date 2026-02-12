#include <syscall.h>

long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;

    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

int fs_ls(void)
{
    return (int) syscall(SYS_FS_LS, 0, 0, 0, 0, 0, 0);
}

int fs_read(const char* name, void* buf, size_t buf_size, size_t* out_size)
{
    return (int) syscall(SYS_FS_READ, (long) name, (long) buf, (long) buf_size,
                         (long) out_size, 0, 0);
}

int fs_create(const char* name, const void* data, size_t size)
{
    return (int) syscall(SYS_FS_CREATE, (long) name, (long) data, (long) size,
                         0, 0, 0);
}

int sys_sleep_ms(uint32_t ms)
{
    return (int) syscall(SYS_SLEEP_MS, (long) ms, 0, 0, 0, 0, 0);
}

uint64_t sys_tick_get(void)
{
    return (uint64_t) syscall(SYS_TICK_GET, 0, 0, 0, 0, 0, 0);
}

int sys_cpu_info_get(syscall_cpu_info_t* out_info)
{
    return (int) syscall(SYS_CPU_INFO_GET, (long) out_info, 0, 0, 0, 0, 0);
}

int sys_sched_info_get(syscall_sched_info_t* out_info)
{
    return (int) syscall(SYS_SCHED_INFO_GET, (long) out_info, 0, 0, 0, 0, 0);
}

int sys_ahci_irq_info_get(syscall_ahci_irq_info_t* out_info)
{
    return (int) syscall(SYS_AHCI_IRQ_INFO_GET, (long) out_info, 0, 0, 0, 0, 0);
}

int sys_rcu_sync(void)
{
    return (int) syscall(SYS_RCU_SYNC, 0, 0, 0, 0, 0, 0);
}

int sys_rcu_info_get(syscall_rcu_info_t* out_info)
{
    return (int) syscall(SYS_RCU_INFO_GET, (long) out_info, 0, 0, 0, 0, 0);
}
