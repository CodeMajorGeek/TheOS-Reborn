#include <Task/Task.h>

#include <CPU/APIC.h>
#include <CPU/GDT.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <CPU/TSS.h>
#include <Debug/Spinlock.h>

#include <string.h>

#define TASK_MAX_CPUS 256
#define TASK_RUNQUEUE_CAPACITY 256
#define TASK_IDLE_BATCH 32U
#define TASK_CPU_LOCAL_CPU_INDEX_OFF 8

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
} task_work_item_t;

typedef struct task_runqueue
{
    spinlock_t lock;
    task_work_item_t items[TASK_RUNQUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    volatile uint32_t count;
} task_runqueue_t;

// TSS must be aligned on 16 bytes for x86-64.
__attribute__((aligned(16))) static TSS_t tss_per_cpu[TASK_MAX_CPUS];
static task_cpu_local_t task_cpu_local_per_apic[TASK_MAX_CPUS];
static task_runqueue_t task_runqueues[TASK_MAX_CPUS];
static volatile uint64_t task_sched_tick_kicks = 0;

static task_t kernel_task;

static task_t* current_task;
static task_t* next_task;

static uint32_t task_current_cpu_index_from_gs(void)
{
    uint32_t cpu_index = TASK_MAX_CPUS;
    __asm__ __volatile__("movl %%gs:%c1, %0"
                         : "=r"(cpu_index)
                         : "i"(TASK_CPU_LOCAL_CPU_INDEX_OFF));
    return cpu_index;
}

static uint32_t task_current_cpu_index(void)
{
    uint32_t gs_cpu_index = task_current_cpu_index_from_gs();
    if (gs_cpu_index < TASK_MAX_CPUS)
        return gs_cpu_index;

    uint32_t smp_cpu_index = SMP_get_current_cpu();
    if (smp_cpu_index >= TASK_MAX_CPUS)
        return 0;
    return smp_cpu_index;
}

uint32_t task_get_current_cpu_index(void)
{
    return task_current_cpu_index();
}

static void task_kick_cpu(uint32_t cpu_index)
{
    if (!APIC_is_enabled() || cpu_index >= TASK_MAX_CPUS)
        return;

    if (cpu_index == task_current_cpu_index())
        return;

    uint8_t apic_id = APIC_get_core_id((uint8_t) cpu_index);
    if (apic_id == 0xFF || !SMP_is_apic_online(apic_id))
        return;

    (void) APIC_send_ipi(apic_id, SMP_IPI_VECTOR_SCHED);
}

static bool task_runqueue_pop_cpu(uint32_t cpu_index, task_work_item_t* out)
{
    if (!out || cpu_index >= TASK_MAX_CPUS)
        return false;

    bool has_item = false;
    task_runqueue_t* rq = &task_runqueues[cpu_index];

    uint64_t flags = spin_lock_irqsave(&rq->lock);
    if (rq->count != 0)
    {
        task_work_item_t* item = &rq->items[rq->head];
        out->fn = item->fn;
        out->arg = item->arg;
        rq->head = (rq->head + 1U) % TASK_RUNQUEUE_CAPACITY;
        rq->count--;
        has_item = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    return has_item;
}

void task_init(uintptr_t kernel_stack)
{
    memset(&kernel_task, 0, sizeof (kernel_task));
    memset(task_cpu_local_per_apic, 0, sizeof(task_cpu_local_per_apic));
    memset(task_runqueues, 0, sizeof(task_runqueues));

    kernel_task.pid = 0;
    kernel_task.ppid = 0;
    kernel_task.stack = kernel_stack;

    current_task = &kernel_task;
    next_task = &kernel_task;

    for (uint32_t cpu = 0; cpu < TASK_MAX_CPUS; cpu++)
    {
        spinlock_init(&task_runqueues[cpu].lock);
        task_runqueues[cpu].head = 0;
        task_runqueues[cpu].tail = 0;
        task_runqueues[cpu].count = 0;
    }
    task_sched_tick_kicks = 0;

    (void) task_init_cpu(0, kernel_stack, APIC_get_bsp_lapic_id());
}

bool task_init_cpu(uint32_t cpu_index, uintptr_t kernel_stack, uint8_t apic_id)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return false;

    TSS_t* tss = &tss_per_cpu[cpu_index];
    memset(tss, 0, sizeof (*tss));

    // Align rsp0 to 16 bytes (required for x86-64)
    // rsp0 is used by CPU for stack switch when interrupt occurs in user mode
    // IMPORTANT: rsp0 must point to a valid, aligned kernel stack
    tss->rsp0 = kernel_stack & ~0xF;
    
    // rsp1 and rsp2 are not used for IRQ handling, leave them zero
    tss->rsp1 = 0;
    tss->rsp2 = 0;
    
    // Verify rsp0 is properly aligned and not NULL
    if (tss->rsp0 == 0 || (tss->rsp0 & 0xF) != 0)
    {
        // This should never happen, but check anyway
        // If rsp0 is invalid, CPU will #GP when trying to use TSS
        return false;
    }

    GDT_load_TSS_segment(tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);

    if (apic_id == 0xFF)
        apic_id = APIC_get_core_id((uint8_t) cpu_index);
    if (apic_id == 0xFF)
        apic_id = APIC_get_current_lapic_id();
    if (apic_id < TASK_MAX_CPUS)
    {
        task_cpu_local_t* cpu_local = &task_cpu_local_per_apic[apic_id];
        cpu_local->syscall_rsp0 = tss->rsp0;
        cpu_local->cpu_index = cpu_index;
        cpu_local->apic_id = apic_id;

        // SYSCALL stub uses SWAPGS + GS:[TASK_CPU_LOCAL_SYSCALL_RSP0_OFF].
        MSR_set(IA32_GS_BASE, (uint64_t) (uintptr_t) cpu_local);
        MSR_set(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) cpu_local);
    }

    return true;
}

bool task_schedule_work(task_work_fn_t fn, void* arg)
{
    return task_schedule_work_on_cpu(task_current_cpu_index(), fn, arg);
}

bool task_schedule_work_on_cpu(uint32_t cpu_index, task_work_fn_t fn, void* arg)
{
    if (!fn || cpu_index >= TASK_MAX_CPUS)
        return false;

    bool queued = false;
    task_runqueue_t* rq = &task_runqueues[cpu_index];

    uint64_t flags = spin_lock_irqsave(&rq->lock);
    if (rq->count < TASK_RUNQUEUE_CAPACITY)
    {
        task_work_item_t* slot = &rq->items[rq->tail];
        slot->fn = fn;
        slot->arg = arg;

        rq->tail = (rq->tail + 1U) % TASK_RUNQUEUE_CAPACITY;
        rq->count++;
        queued = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    if (queued)
        task_kick_cpu(cpu_index);

    return queued;
}

bool task_run_next_work(void)
{
    return task_run_next_work_on_cpu(task_current_cpu_index());
}

bool task_run_next_work_on_cpu(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return false;

    task_work_item_t work = { 0 };
    if (!task_runqueue_pop_cpu(cpu_index, &work))
        return false;

    work.fn(work.arg);
    return true;
}

void task_scheduler_on_tick(void)
{
    if (!APIC_is_enabled())
        return;

    if (APIC_get_current_lapic_id() != APIC_get_bsp_lapic_id())
        return;

    if (__atomic_load_n(&task_runqueues[0].count, __ATOMIC_RELAXED) == 0)
        return;

    ++task_sched_tick_kicks;
}

uint32_t task_runqueue_depth(void)
{
    return task_runqueue_depth_cpu(task_current_cpu_index());
}

uint32_t task_runqueue_depth_cpu(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return 0;

    return __atomic_load_n(&task_runqueues[cpu_index].count, __ATOMIC_RELAXED);
}

uint32_t task_runqueue_depth_total(void)
{
    uint32_t total = 0;
    for (uint32_t cpu = 0; cpu < TASK_MAX_CPUS; cpu++)
        total += __atomic_load_n(&task_runqueues[cpu].count, __ATOMIC_RELAXED);

    return total;
}

__attribute__((__noreturn__)) void task_idle_loop(void)
{
    while (true)
    {
        bool ran_work = false;
        for (uint32_t i = 0; i < TASK_IDLE_BATCH; i++)
        {
            if (!task_run_next_work())
                break;
            ran_work = true;
        }

        if (ran_work)
            continue;

        __asm__ __volatile__("sti\nhlt");
    }
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
    uint32_t cpu_index = SMP_get_current_cpu();
    if (cpu_index >= TASK_MAX_CPUS)
        cpu_index = 0;
    TSS_t* cpu_tss = &tss_per_cpu[cpu_index];
    cpu_tss->rsp0 = (current_task->stack & ~0xF);

    uint8_t apic_id = APIC_get_current_lapic_id();
    if (apic_id < TASK_MAX_CPUS)
        task_cpu_local_per_apic[apic_id].syscall_rsp0 = cpu_tss->rsp0;
    
    // IMPORTANT: After modifying TSS, we must reload it in TR
    // The CPU caches TSS data, so changes require a reload
    GDT_load_TSS_segment(cpu_tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);
    
    // Jump (not call) to avoid leaving a return address on the stack.
    // A stray return address corrupts the interrupt frame and breaks iretq.
    __asm__ __volatile__("jmp perform_task_switch" : : "D"(current_task) : "memory");
    __builtin_unreachable();
}
