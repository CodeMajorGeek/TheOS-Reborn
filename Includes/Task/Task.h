#ifndef _TASK_H
#define _TASK_H

#include <stdint.h>
#include <stdbool.h>

#define KERNEL_STACK_SIZE   0x2000      // Define a 16 kB stack area for the kernel.

#define TASK_SWITCH_APPENED 0xDEADBEEF  // A dummy value to know when we have already switched to another task.

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
uint32_t task_runqueue_depth(void);
uint32_t task_runqueue_depth_cpu(uint32_t cpu_index);
uint32_t task_runqueue_depth_total(void);
__attribute__((__noreturn__)) void task_idle_loop(void);

#endif
