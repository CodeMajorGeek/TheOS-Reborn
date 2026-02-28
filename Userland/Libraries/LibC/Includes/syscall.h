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

long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6);

int fs_ls(void);
int fs_ls_path(const char* path);
int fs_is_dir(const char* path);
int fs_read(const char* name, void* buf, size_t buf_size, size_t* out_size);
int fs_create(const char* name, const void* data, size_t size);
int fs_mkdir(const char* path);
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
int fs_write(int fd, const void* buf, size_t len);
int64_t fs_seek(int fd, int64_t offset, int whence);
int sys_kbd_get_scancode(void);
int sys_waitpid(int pid, int* out_status, int* out_signal);

#endif
