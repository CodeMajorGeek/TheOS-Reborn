#ifndef _UAPI_SYSCALL_H
#define _UAPI_SYSCALL_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

#define SYS_SLEEP_MS          1
#define SYS_TICK_GET          2
#define SYS_CPU_INFO_GET      3
#define SYS_SCHED_INFO_GET    4
#define SYS_AHCI_IRQ_INFO_GET 5
#define SYS_RCU_SYNC          6
#define SYS_RCU_INFO_GET      7
#define SYS_CONSOLE_WRITE     8
#define SYS_EXIT              9
#define SYS_FORK              10
#define SYS_EXECVE            11
#define SYS_YIELD             12
#define SYS_MAP               13
#define SYS_UNMAP             14
#define SYS_MPROTECT          15
#define SYS_OPEN              16
#define SYS_CLOSE             17
#define SYS_READ              18
#define SYS_WRITE             19
#define SYS_LSEEK             20
#define SYS_KBD_GET_SCANCODE  21
#define SYS_FS_ISDIR          22
#define SYS_FS_MKDIR          23
#define SYS_FS_READDIR        24
#define SYS_WAITPID           25
#define SYS_KILL              26
#define SYS_POWER             27

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
#define SYS_SIGKILL      9
#define SYS_SIGSEGV      11
#define SYS_SIGFAULT     128

#define SYS_SLEEP_STATE_S0 0U
#define SYS_SLEEP_STATE_S1 1U
#define SYS_SLEEP_STATE_S2 2U
#define SYS_SLEEP_STATE_S3 3U
#define SYS_SLEEP_STATE_S4 4U
#define SYS_SLEEP_STATE_S5 5U

#define SYS_POWER_CMD_SHUTDOWN 1U
#define SYS_POWER_CMD_SLEEP    2U
#define SYS_POWER_CMD_REBOOT   3U

#define SYS_DIRENT_NAME_MAX 255U
#define SYS_DT_UNKNOWN      0U
#define SYS_DT_DIR          4U
#define SYS_DT_REG          8U

#ifndef __ASSEMBLER__
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

typedef struct syscall_dirent
{
    uint32_t d_ino;
    uint8_t d_type;
    uint8_t reserved[3];
    char d_name[SYS_DIRENT_NAME_MAX + 1U];
} syscall_dirent_t;
#endif

#endif
