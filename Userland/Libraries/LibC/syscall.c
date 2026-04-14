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

int sys_proc_info_get(syscall_proc_info_t* out_entries, uint32_t max_entries, uint32_t* out_total)
{
    return (int) syscall(SYS_PROC_INFO_GET,
                         (long) out_entries,
                         (long) max_entries,
                         (long) out_total,
                         0,
                         0,
                         0);
}

int sys_console_write(const void* buf, size_t len)
{
    return (int) syscall(SYS_CONSOLE_WRITE, (long) buf, (long) len, 0, 0, 0, 0);
}

int sys_console_route_set(uint32_t flags)
{
    return (int) syscall(SYS_CONSOLE_ROUTE_SET, (long) flags, 0, 0, 0, 0, 0);
}

int sys_console_route_read(void* buf, size_t len)
{
    return (int) syscall(SYS_CONSOLE_ROUTE_READ, (long) buf, (long) len, 0, 0, 0, 0);
}

int sys_console_route_set_sid(uint32_t console_sid, uint32_t flags)
{
    return (int) syscall(SYS_CONSOLE_ROUTE_SET_SID, (long) console_sid, (long) flags, 0, 0, 0, 0);
}

int sys_console_route_read_sid(uint32_t console_sid, void* buf, size_t len)
{
    return (int) syscall(SYS_CONSOLE_ROUTE_READ_SID, (long) console_sid, (long) buf, (long) len, 0, 0, 0);
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

void* sys_map_ex(void* addr, size_t len, uint64_t prot, uint64_t flags, int fd, uint64_t offset)
{
    long ret = syscall(SYS_MAP, (long) addr, (long) len, (long) prot, (long) flags, (long) fd, (long) offset);
    if (ret < 0)
        return NULL;

    return (void*) (uintptr_t) ret;
}

void* sys_map(void* addr, size_t len, uint64_t prot)
{
    return sys_map_ex(addr, len, prot, SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
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

int sys_ioctl(int fd, unsigned long request, void* arg)
{
    return (int) syscall(SYS_IOCTL, (long) fd, (long) request, (long) arg, 0, 0, 0);
}

int sys_socket(int domain, int type, int protocol)
{
    return (int) syscall(SYS_SOCKET, (long) domain, (long) type, (long) protocol, 0, 0, 0);
}

int sys_bind(int fd, const void* addr, size_t addrlen)
{
    return (int) syscall(SYS_BIND, (long) fd, (long) addr, (long) addrlen, 0, 0, 0);
}

int sys_sendto(int fd, const void* buf, size_t len, int flags, const void* dest_addr, size_t addrlen)
{
    return (int) syscall(SYS_SENDTO,
                         (long) fd,
                         (long) buf,
                         (long) len,
                         (long) flags,
                         (long) dest_addr,
                         (long) addrlen);
}

int sys_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr, void* addrlen_ptr)
{
    return (int) syscall(SYS_RECVFROM,
                         (long) fd,
                         (long) buf,
                         (long) len,
                         (long) flags,
                         (long) src_addr,
                         (long) addrlen_ptr);
}

int sys_connect(int fd, const void* addr, size_t addrlen)
{
    return (int) syscall(SYS_CONNECT, (long) fd, (long) addr, (long) addrlen, 0, 0, 0);
}

int sys_getsockname(int fd, void* addr, void* addrlen_ptr)
{
    return (int) syscall(SYS_GETSOCKNAME, (long) fd, (long) addr, (long) addrlen_ptr, 0, 0, 0);
}

int sys_getpeername(int fd, void* addr, void* addrlen_ptr)
{
    return (int) syscall(SYS_GETPEERNAME, (long) fd, (long) addr, (long) addrlen_ptr, 0, 0, 0);
}

int sys_listen(int fd, int backlog)
{
    return (int) syscall(SYS_LISTEN, (long) fd, (long) backlog, 0, 0, 0, 0);
}

int sys_accept(int fd, void* addr, void* addrlen_ptr)
{
    return (int) syscall(SYS_ACCEPT, (long) fd, (long) addr, (long) addrlen_ptr, 0, 0, 0);
}

int sys_kbd_get_scancode(void)
{
    return (int) syscall(SYS_KBD_GET_SCANCODE, 0, 0, 0, 0, 0, 0);
}

int sys_kbd_inject_scancode(uint32_t target_pid, uint8_t scancode)
{
    return (int) syscall(SYS_KBD_INJECT_SCANCODE, (long) target_pid, (long) scancode, 0, 0, 0, 0);
}

int sys_mouse_get_event(syscall_mouse_event_t* out_event)
{
    return (int) syscall(SYS_MOUSE_GET_EVENT, (long) out_event, 0, 0, 0, 0, 0);
}

int sys_mouse_debug_info_get(syscall_mouse_debug_info_t* out_info)
{
    return (int) syscall(SYS_MOUSE_DEBUG_INFO_GET, (long) out_info, 0, 0, 0, 0, 0);
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

int sys_thread_create_ex(uintptr_t start_rip, uintptr_t arg, uintptr_t stack_top, uintptr_t fs_base)
{
    return (int) syscall(SYS_THREAD_CREATE,
                         (long) start_rip,
                         (long) arg,
                         (long) stack_top,
                         (long) fs_base,
                         0,
                         0);
}

int sys_thread_create(uintptr_t start_rip, uintptr_t arg, uintptr_t stack_top)
{
    return sys_thread_create_ex(start_rip, arg, stack_top, 0);
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

int sys_thread_set_fsbase(uintptr_t fs_base)
{
    return (int) syscall(SYS_THREAD_SET_FSBASE, (long) fs_base, 0, 0, 0, 0, 0);
}

uintptr_t sys_thread_get_fsbase(void)
{
    long ret = syscall(SYS_THREAD_GET_FSBASE, 0, 0, 0, 0, 0, 0);
    if (ret < 0)
        return 0;
    return (uintptr_t) ret;
}
