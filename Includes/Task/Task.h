#ifndef _TASK_H
#define _TASK_H

#include <stdint.h>

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

extern uint64_t read_rip(void);
extern uint64_t read_flags(void);
extern void perform_task_switch(task_t* task);

void task_init(uintptr_t kernel_stack);
void task_switch(void);

#endif