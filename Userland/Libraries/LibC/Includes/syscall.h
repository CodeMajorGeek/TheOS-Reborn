#ifndef _SYSCALL_H
#define _SYSCALL_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#endif
#include "../../../../Includes/UAPI/Syscall.h"

#ifndef __ASSEMBLER__

static inline long syscall(long num, long a1, long a2, long a3,
                           long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}


int fs_is_dir(const char* path);
int fs_mkdir(const char* path);
int fs_readdir(const char* path, uint64_t index, syscall_dirent_t* out_entry);
int sys_sleep_ms(uint32_t ms);
uint64_t sys_tick_get(void);
int sys_cpu_info_get(syscall_cpu_info_t* out_info);
int sys_sched_info_get(syscall_sched_info_t* out_info);
int sys_ahci_irq_info_get(syscall_ahci_irq_info_t* out_info);
int sys_rcu_sync(void);
int sys_rcu_info_get(syscall_rcu_info_t* out_info);
int sys_proc_info_get(syscall_proc_info_t* out_entries, uint32_t max_entries, uint32_t* out_total);
int sys_console_write(const void* buf, size_t len);
int sys_console_route_set(uint32_t flags);
int sys_console_route_read(void* buf, size_t len);
int sys_console_route_set_sid(uint32_t console_sid, uint32_t flags);
int sys_console_route_read_sid(uint32_t console_sid, void* buf, size_t len);
int sys_console_route_input_write_sid(uint32_t console_sid, const void* buf, size_t len);
int sys_console_route_input_read(void* buf, size_t len);
__attribute__((__noreturn__)) void sys_exit(int status);
int sys_fork(void);
int sys_execve(const char* path, const char* const argv[], const char* const envp[]);
int sys_yield(void);
void* sys_map_ex(void* addr, size_t len, uint64_t prot, uint64_t flags, int fd, uint64_t offset);
void* sys_map(void* addr, size_t len, uint64_t prot);
int sys_unmap(void* addr, size_t len);
int sys_mprotect(void* addr, size_t len, uint64_t prot);
int sys_open(const char* path, uint64_t flags);
int sys_close(int fd);
int sys_read(int fd, void* buf, size_t len);
int sys_write(int fd, const void* buf, size_t len);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int sys_ioctl(int fd, unsigned long request, void* arg);
int sys_socket(int domain, int type, int protocol);
int sys_bind(int fd, const void* addr, size_t addrlen);
int sys_sendto(int fd, const void* buf, size_t len, int flags, const void* dest_addr, size_t addrlen);
int sys_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr, void* addrlen_ptr);
int sys_connect(int fd, const void* addr, size_t addrlen);
int sys_getsockname(int fd, void* addr, void* addrlen_ptr);
int sys_getpeername(int fd, void* addr, void* addrlen_ptr);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, void* addr, void* addrlen_ptr);
int sys_kbd_get_scancode(void);
int sys_kbd_inject_scancode(uint32_t target_pid, uint8_t scancode);
int sys_kbd_capture_set(uint32_t owner_pid_or_zero_to_release);
int sys_mouse_get_event(syscall_mouse_event_t* out_event);
int sys_mouse_debug_info_get(syscall_mouse_debug_info_t* out_info);
int sys_waitpid(int pid, int* out_status, int* out_signal);
int sys_kill(int pid, int signal);
int sys_power(uint32_t cmd, uint32_t arg);
int sys_shutdown(void);
int sys_sleep(uint32_t state);
int sys_reboot(void);
int sys_thread_create_ex(uintptr_t start_rip, uintptr_t arg, uintptr_t stack_top, uintptr_t fs_base);
int sys_thread_create(uintptr_t start_rip, uintptr_t arg, uintptr_t stack_top);
int sys_thread_join(int tid, uint64_t* out_retval);
__attribute__((__noreturn__)) void sys_thread_exit(uint64_t retval);
int sys_thread_self(void);
int sys_thread_set_fsbase(uintptr_t fs_base);
uintptr_t sys_thread_get_fsbase(void);
int sys_pipe(int* pipefd);
int sys_futex(int* uaddr, int op, int val, unsigned int timeout_ms);
int sys_shmget(int key, size_t size, int shmflg);
long sys_shmat(int shmid, const void* shmaddr);
int sys_shmdt(const void* shmaddr);
int sys_shmctl(int shmid, int cmd);
int sys_msgget(int key, int msgflg);
int sys_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg);
long sys_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);
#endif

#endif
