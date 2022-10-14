#include <Task/Task.h>

#include <Memory/KMem.h>
#include <CPU/GDT.h>
#include <CPU/TSS.h>

#include <string.h>
#include <stdio.h>

static TSS_t tss;

static task_t kernel_task;

static task_t* current_task;
static task_t* next_task;

void task_init(uintptr_t kernel_stack)
{
    memset(&tss, 0, sizeof (tss));
    memset(&kernel_task, 0, sizeof (kernel_task));

    kernel_task.pid = 0;
    kernel_task.ppid = 0;
    kernel_task.stack = kernel_stack;

    tss.rsp0 = kernel_stack;

    GDT_load_TSS_segment(&tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);

    current_task = &kernel_task;
    next_task = &kernel_task;
}

void task_switch(void)
{
    __asm__ __volatile__ ("movq %%rbp, %0" : "=r" (current_task->rbp));
    __asm__ __volatile__ ("movq %%cr3, %0" : "=r" (current_task->cr3));
    __asm__ __volatile__ ("movq %%rax, %0" : "=r" (current_task->rax));
    __asm__ __volatile__ ("movq %%rcx, %0" : "=r" (current_task->rcx));
    __asm__ __volatile__ ("movq %%rdx, %0" : "=r" (current_task->rdx));
    __asm__ __volatile__ ("movq %%r8, %0" : "=r" (current_task->r8));
    __asm__ __volatile__ ("movq %%r9, %0" : "=r" (current_task->r9));
    __asm__ __volatile__ ("movq %%r10, %0" : "=r" (current_task->r10));
    __asm__ __volatile__ ("movq %%r11, %0" : "=r" (current_task->r11));

    current_task->flags = read_flags();
    current_task->rip = read_rip();

    if (current_task->rip == TASK_SWITCH_APPENED) // Maybe find a better solution than dummy value into rax...
        return;

    current_task = next_task;

    tss.rsp1 = current_task->stack;
    tss.rsp2 = current_task->stack;
    
    perform_task_switch(current_task);
}