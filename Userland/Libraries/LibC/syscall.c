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

int fs_ls_path(const char* path)
{
    return (int) syscall(SYS_FS_LS, (long) path, 0, 0, 0, 0, 0);
}

int fs_is_dir(const char* path)
{
    return (int) syscall(SYS_FS_ISDIR, (long) path, 0, 0, 0, 0, 0);
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

int fs_mkdir(const char* path)
{
    return (int) syscall(SYS_FS_MKDIR, (long) path, 0, 0, 0, 0, 0);
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

int fs_write(int fd, const void* buf, size_t len)
{
    return (int) syscall(SYS_FS_WRITE, (long) fd, (long) buf, (long) len, 0, 0, 0);
}

int64_t fs_seek(int fd, int64_t offset, int whence)
{
    return (int64_t) syscall(SYS_FS_SEEK, (long) fd, (long) offset, (long) whence, 0, 0, 0);
}

int sys_kbd_get_scancode(void)
{
    return (int) syscall(SYS_KBD_GET_SCANCODE, 0, 0, 0, 0, 0, 0);
}

int sys_waitpid(int pid, int* out_status, int* out_signal)
{
    return (int) syscall(SYS_WAITPID, (long) pid, (long) out_status, (long) out_signal, 0, 0, 0);
}
