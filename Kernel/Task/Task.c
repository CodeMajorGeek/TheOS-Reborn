#include <Task/Task.h>

#include <Memory/KMem.h>
#include <CPU/GDT.h>
#include <CPU/TSS.h>

#include <string.h>

// TSS must be aligned on 16 bytes for x86-64
__attribute__((aligned(16))) static TSS_t tss;

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

    // Align rsp0 to 16 bytes (required for x86-64)
    // rsp0 is used by CPU for stack switch when interrupt occurs in user mode
    // IMPORTANT: rsp0 must point to a valid, aligned kernel stack
    tss.rsp0 = kernel_stack & ~0xF;
    
    // rsp1 and rsp2 are not used for IRQ handling, leave them zero
    tss.rsp1 = 0;
    tss.rsp2 = 0;
    
    // Verify rsp0 is properly aligned and not NULL
    if (tss.rsp0 == 0 || (tss.rsp0 & 0xF) != 0)
    {
        // This should never happen, but check anyway
        // If rsp0 is invalid, CPU will #GP when trying to use TSS
        return;
    }

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

    // Update rsp0 for new task's kernel stack
    // rsp1 and rsp2 are not used for IRQ handling
    tss.rsp0 = (current_task->stack & ~0xF);
    
    // IMPORTANT: After modifying TSS, we must reload it in TR
    // The CPU caches TSS data, so changes require a reload
    GDT_load_TSS_segment(&tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);
    
    // Jump (not call) to avoid leaving a return address on the stack.
    // A stray return address corrupts the interrupt frame and breaks iretq.
    __asm__ __volatile__("jmp perform_task_switch" : : "D"(current_task) : "memory");
    __builtin_unreachable();
}
