#ifndef _TASK_H
#define _TASK_H

#include <Debug/Spinlock.h>

#include <stdint.h>
#include <stdbool.h>

#define KERNEL_STACK_SIZE   0x2000      // Define a 16 kB stack area for the kernel.

#define TASK_SWITCH_APPENED 0xDEADBEEF  // A dummy value to know when we have already switched to another task.

#define TASK_MAX_CPUS 256
#define TASK_RUNQUEUE_CAPACITY 256
#define TASK_IDLE_BATCH 32U
#define TASK_CPU_LOCAL_CPU_INDEX_OFF 8
#define TASK_WORK_CPU_ANY 0xFFFFFFFFU
#define TASK_PUSH_TRIGGER_DEPTH 8U
#define TASK_PUSH_IMBALANCE_DELTA 2U
#define TASK_STEAL_MAX_VICTIMS 4U
#define TASK_STEAL_BACKOFF_MIN 64U
#define TASK_STEAL_BACKOFF_MAX 4096U
#define TASK_STATS_LOG_INTERVAL 100U


typedef struct task
{
    uint64_t rip;
    uint64_t rbp;
    uint64_t cr3;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t flags;

    uint32_t pid;
    uint32_t ppid;

    uintptr_t stack;
} task_t;

typedef void (*task_work_fn_t)(void* arg);

typedef struct task_cpu_local
{
    uintptr_t syscall_rsp0;
    uint32_t cpu_index;
    uint8_t apic_id;
    uint8_t reserved[3];
} __attribute__((__packed__)) task_cpu_local_t;

typedef struct task_work_item
{
    task_work_fn_t fn;
    void* arg;
    uint32_t affinity_cpu;
} task_work_item_t;

typedef struct task_runqueue
{
    spinlock_t lock;
    task_work_item_t items[TASK_RUNQUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    volatile uint32_t count;
} task_runqueue_t;

extern uint64_t read_rip(void);
extern uint64_t read_flags(void);
extern void perform_task_switch(task_t* task);
void task_init(uintptr_t kernel_stack);
bool task_init_cpu(uint32_t cpu_index, uintptr_t kernel_stack, uint8_t apic_id);
uint32_t task_get_current_cpu_index(void);
void task_switch(void);
bool task_schedule_work(task_work_fn_t fn, void* arg);
bool task_schedule_work_on_cpu(uint32_t cpu_index, task_work_fn_t fn, void* arg);
bool task_run_next_work(void);
bool task_run_next_work_on_cpu(uint32_t cpu_index);
void task_scheduler_on_tick(void);
void task_set_push_balance(bool enabled);
bool task_is_push_balance_enabled(void);
void task_set_work_stealing(bool enabled);
bool task_is_work_stealing_enabled(void);
uint32_t task_runqueue_depth(void);
uint32_t task_runqueue_depth_cpu(uint32_t cpu_index);
uint32_t task_runqueue_depth_total(void);
__attribute__((__noreturn__)) void task_idle_loop(void);

#endif
