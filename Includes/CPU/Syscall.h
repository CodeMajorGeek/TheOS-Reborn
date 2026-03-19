#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <CPU/ISR.h>
#include <Debug/Spinlock.h>
#include <UAPI/Syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYSCALL_INT 0x80

#define SYSCALL_FMASK_TF_BIT        (1ULL << 8)
#define SYSCALL_FMASK_DF_BIT        (1ULL << 10)
#define SYSCALL_USER_VADDR_MIN         0x0000000020000000ULL
#define SYSCALL_USER_VADDR_MAX         0x00007FFFFFFFFFFFULL
#define SYSCALL_USER_CSTR_MAX          256U
#define SYSCALL_CONSOLE_MAX_WRITE      4096U
#define SYSCALL_PAGE_SIZE              0x1000ULL
#define SYSCALL_MAP_MAX_PAGES          16384U
#define SYSCALL_MAP_HINT_BASE          0x0000000050000000ULL
#define SYSCALL_MAP_HINT_LIMIT         0x000000006F000000ULL
#define SYSCALL_MAX_OPEN_FILES         64U
#define SYSCALL_MAX_PROCS              32U
#define SYSCALL_MAX_EXIT_EVENTS        64U
#define SYSCALL_MAX_THREAD_EXIT_EVENTS 64U
#define SYSCALL_PATH_MAX_COMPONENTS    32U
#define SYSCALL_PATH_COMPONENT_MAX     255U
#define SYSCALL_PROC_NONE              0xFFFFFFFFU
#define SYSCALL_ELF_MAX_SIZE           (16ULL * 1024ULL * 1024ULL)
#define SYSCALL_ELF_MAX_PHDRS          64U
#define SYSCALL_ELF_STACK_TOP          0x0000000070000000ULL
#define SYSCALL_ELF_STACK_SIZE         (512ULL * 1024ULL)
#define SYSCALL_EXEC_MAX_ARGS          64U
#define SYSCALL_EXEC_MAX_ENVP          64U
#define SYSCALL_ELF_CANONICAL_LOW_MAX  0x0000800000000000ULL
#define SYSCALL_PTE_PS                 (1ULL << 7)
#define SYSCALL_PTE_COW                (1ULL << 9)
#define SYSCALL_PTE_DMABUF             (1ULL << 10)
#define SYSCALL_ELF_PF_X               (1U << 0)
#define SYSCALL_ELF_PF_W               (1U << 1)
#define SYSCALL_PAGE_FAULT_PRESENT     (1ULL << 0)
#define SYSCALL_PAGE_FAULT_WRITE       (1ULL << 1)
#define SYSCALL_PREEMPT_QUANTUM_TICKS  4U
#define SYSCALL_COW_MAX_REFS           32768U
#define SYSCALL_RFLAGS_IF              (1ULL << 9)

#define SYSCALL_FD_TYPE_NONE     0U
#define SYSCALL_FD_TYPE_REGULAR  1U
#define SYSCALL_FD_TYPE_DRM_CARD 2U
#define SYSCALL_FD_TYPE_DMABUF   3U
#define SYSCALL_FD_TYPE_AUDIO_DSP 4U

extern void enable_syscall_ext(void);
extern void syscall_handler_stub(void);

typedef struct syscall_frame
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t reserved0;
} syscall_frame_t;

typedef struct syscall_file_desc
{
    bool used;
    uint32_t type;
    uint32_t owner_pid;
    uint32_t drm_file_id;
    uint32_t drm_dmabuf_id;
    bool can_read;
    bool can_write;
    bool exclusive;
    bool dirty;
    bool io_busy;
    char path[SYSCALL_USER_CSTR_MAX];
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t offset;
    size_t max_size;
} syscall_file_desc_t;

typedef struct syscall_process
{
    bool used;
    bool exiting;
    bool terminated_by_signal;
    bool owns_cr3;
    bool is_thread;
    uint32_t pid;
    uint32_t ppid;
    uint32_t owner_pid;
    int64_t exit_status;
    uint64_t thread_exit_value;
    int32_t term_signal;
    uintptr_t cr3_phys;
    uintptr_t fs_base;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t pending_rax;
} syscall_process_t;

typedef struct syscall_cow_ref
{
    bool used;
    uintptr_t phys;
    uint32_t refs;
} syscall_cow_ref_t;

typedef struct syscall_exit_event
{
    bool used;
    uint32_t ppid;
    uint32_t pid;
    int64_t status;
    int32_t signal;
} syscall_exit_event_t;

typedef struct syscall_thread_exit_event
{
    bool used;
    uint32_t owner_pid;
    uint32_t tid;
    uint64_t value;
} syscall_thread_exit_event_t;

typedef struct syscall_elf64_ehdr
{
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) syscall_elf64_ehdr_t;

typedef struct syscall_elf64_phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) syscall_elf64_phdr_t;

typedef struct syscall_runtime_state
{
    volatile uint64_t count_per_cpu[256];
    uintptr_t user_map_hint;
    syscall_file_desc_t fds[SYSCALL_MAX_OPEN_FILES];
    spinlock_t fd_lock;
    bool fd_lock_ready;
    spinlock_t fs_lock;
    bool fs_lock_ready;
    spinlock_t vm_lock;
    bool vm_lock_ready;
    syscall_process_t procs[SYSCALL_MAX_PROCS];
    syscall_exit_event_t exit_events[SYSCALL_MAX_EXIT_EVENTS];
    syscall_thread_exit_event_t thread_exit_events[SYSCALL_MAX_THREAD_EXIT_EVENTS];
    syscall_cow_ref_t cow_refs[SYSCALL_COW_MAX_REFS];
    uint32_t cpu_current_proc[256];
    uint8_t cpu_need_resched[256];
    uint8_t cpu_slice_ticks[256];
    uint32_t next_pid;
    spinlock_t proc_lock;
    bool proc_lock_ready;
    spinlock_t cow_lock;
    bool cow_lock_ready;
} syscall_runtime_state_t;

void Syscall_init(void);

uint64_t Syscall_interrupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index);
uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame, uint32_t cpu_index);
uint64_t Syscall_post_handler(uint64_t syscall_ret, syscall_frame_t* frame, uint32_t cpu_index);
bool Syscall_handle_user_exception(interrupt_frame_t* frame, uintptr_t fault_addr);
void Syscall_on_timer_tick(uint32_t cpu_index);
bool Syscall_handle_timer_preempt(interrupt_frame_t* frame, uint32_t cpu_index);

#endif
