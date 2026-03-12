#ifndef _SYSCALL_H
#define _SYSCALL_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#endif
#include "../../../../Includes/UAPI/Syscall.h"

#ifndef __ASSEMBLER__
long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6);

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
int sys_console_write(const void* buf, size_t len);
__attribute__((__noreturn__)) void sys_exit(int status);
int sys_fork(void);
int sys_execve(const char* path, const char* const argv[], const char* const envp[]);
int sys_yield(void);
void* sys_map(void* addr, size_t len, uint64_t prot);
int sys_unmap(void* addr, size_t len);
int sys_mprotect(void* addr, size_t len, uint64_t prot);
int sys_open(const char* path, uint64_t flags);
int sys_close(int fd);
int sys_read(int fd, void* buf, size_t len);
int sys_write(int fd, const void* buf, size_t len);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int sys_kbd_get_scancode(void);
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
#endif

#endif
