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

int fs_is_dir(const char* path)
{
    return (int) syscall(SYS_FS_ISDIR, (long) path, 0, 0, 0, 0, 0);
}

int fs_mkdir(const char* path)
{
    return (int) syscall(SYS_FS_MKDIR, (long) path, 0, 0, 0, 0, 0);
}

int fs_readdir(const char* path, uint64_t index, syscall_dirent_t* out_entry)
{
    return (int) syscall(SYS_FS_READDIR, (long) path, (long) index, (long) out_entry, 0, 0, 0);
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

int sys_console_write(const void* buf, size_t len)
{
    return (int) syscall(SYS_CONSOLE_WRITE, (long) buf, (long) len, 0, 0, 0, 0);
}

__attribute__((__noreturn__)) void sys_exit(int status)
{
    (void) syscall(SYS_EXIT, (long) status, 0, 0, 0, 0, 0);

    for (;;)
        (void) syscall(SYS_SLEEP_MS, 1000, 0, 0, 0, 0, 0);
}

int sys_fork(void)
{
    return (int) syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

int sys_execve(const char* path, const char* const argv[], const char* const envp[])
{
    return (int) syscall(SYS_EXECVE, (long) path, (long) argv, (long) envp, 0, 0, 0);
}

int sys_yield(void)
{
    return (int) syscall(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

void* sys_map(void* addr, size_t len, uint64_t prot)
{
    long ret = syscall(SYS_MAP, (long) addr, (long) len, (long) prot, 0, 0, 0);
    if (ret < 0)
        return NULL;

    return (void*) (uintptr_t) ret;
}

int sys_unmap(void* addr, size_t len)
{
    return (int) syscall(SYS_UNMAP, (long) addr, (long) len, 0, 0, 0, 0);
}

int sys_mprotect(void* addr, size_t len, uint64_t prot)
{
    return (int) syscall(SYS_MPROTECT, (long) addr, (long) len, (long) prot, 0, 0, 0);
}

int sys_open(const char* path, uint64_t flags)
{
    return (int) syscall(SYS_OPEN, (long) path, (long) flags, 0, 0, 0, 0);
}

int sys_close(int fd)
{
    return (int) syscall(SYS_CLOSE, (long) fd, 0, 0, 0, 0, 0);
}

int sys_read(int fd, void* buf, size_t len)
{
    return (int) syscall(SYS_READ, (long) fd, (long) buf, (long) len, 0, 0, 0);
}

int sys_write(int fd, const void* buf, size_t len)
{
    return (int) syscall(SYS_WRITE, (long) fd, (long) buf, (long) len, 0, 0, 0);
}

int64_t sys_lseek(int fd, int64_t offset, int whence)
{
    return (int64_t) syscall(SYS_LSEEK, (long) fd, (long) offset, (long) whence, 0, 0, 0);
}

int sys_kbd_get_scancode(void)
{
    return (int) syscall(SYS_KBD_GET_SCANCODE, 0, 0, 0, 0, 0, 0);
}

int sys_waitpid(int pid, int* out_status, int* out_signal)
{
    return (int) syscall(SYS_WAITPID, (long) pid, (long) out_status, (long) out_signal, 0, 0, 0);
}

int sys_kill(int pid, int signal)
{
    return (int) syscall(SYS_KILL, (long) pid, (long) signal, 0, 0, 0, 0);
}

int sys_power(uint32_t cmd, uint32_t arg)
{
    return (int) syscall(SYS_POWER, (long) cmd, (long) arg, 0, 0, 0, 0);
}

int sys_shutdown(void)
{
    return sys_power(SYS_POWER_CMD_SHUTDOWN, 0);
}

int sys_sleep(uint32_t state)
{
    return sys_power(SYS_POWER_CMD_SLEEP, state);
}

int sys_reboot(void)
{
    return sys_power(SYS_POWER_CMD_REBOOT, 0);
}

int sys_thread_create(uintptr_t start_rip, uintptr_t arg, uintptr_t stack_top)
{
    return (int) syscall(SYS_THREAD_CREATE, (long) start_rip, (long) arg, (long) stack_top, 0, 0, 0);
}

int sys_thread_join(int tid, uint64_t* out_retval)
{
    return (int) syscall(SYS_THREAD_JOIN, (long) tid, (long) out_retval, 0, 0, 0, 0);
}

__attribute__((__noreturn__)) void sys_thread_exit(uint64_t retval)
{
    (void) syscall(SYS_THREAD_EXIT, (long) retval, 0, 0, 0, 0, 0);
    for (;;)
        (void) syscall(SYS_SLEEP_MS, 1000, 0, 0, 0, 0, 0);
}

int sys_thread_self(void)
{
    return (int) syscall(SYS_THREAD_SELF, 0, 0, 0, 0, 0, 0);
}
