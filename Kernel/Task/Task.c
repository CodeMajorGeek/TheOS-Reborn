#include <Task/Task.h>

#include <CPU/APIC.h>
#include <CPU/GDT.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <CPU/TSS.h>
#include <Debug/KDebug.h>

#include <string.h>

// TSS must be aligned on 16 bytes for x86-64.
__attribute__((aligned(16))) static TSS_t tss_per_cpu[TASK_MAX_CPUS];
static task_cpu_local_t task_cpu_local_per_apic[TASK_MAX_CPUS];
static task_runqueue_t task_runqueues[TASK_MAX_CPUS];
static uint32_t task_steal_cursor[TASK_MAX_CPUS];
static uint64_t task_exec_runs[TASK_MAX_CPUS];
static uint64_t task_exec_runs_total = 0;
static uint64_t task_steal_runs[TASK_MAX_CPUS];
static uint64_t task_steal_fail_runs[TASK_MAX_CPUS];
static uint64_t task_steal_from_cpu[TASK_MAX_CPUS];
static uint64_t task_idle_hlt_runs[TASK_MAX_CPUS];
static uint32_t task_last_steal_victim[TASK_MAX_CPUS];
static uint64_t task_stats_log_epoch = 0;
static uint8_t task_push_balance_enabled = 1;
static uint8_t task_work_stealing_enabled = 1;
static volatile uint64_t task_sched_tick_kicks = 0;

static task_t kernel_task;

static task_t* current_task;
static task_t* next_task;
static bool task_cpu_is_online(uint32_t cpu_index);

static inline void task_pause(void)
{
    __asm__ __volatile__("pause");
}

static void task_maybe_log_cpu_stats(uint64_t total_runs_trigger)
{
    if (total_runs_trigger == 0 || (total_runs_trigger % TASK_STATS_LOG_INTERVAL) != 0)
        return;

    uint64_t epoch = total_runs_trigger / TASK_STATS_LOG_INTERVAL;
    uint64_t observed = __atomic_load_n(&task_stats_log_epoch, __ATOMIC_RELAXED);
    while (epoch > observed)
    {
        if (!__atomic_compare_exchange_n(&task_stats_log_epoch,
                                         &observed,
                                         epoch,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED))
            continue;

        uint8_t core_count = APIC_get_core_count();
        uint32_t max_cpu = core_count;
        if (max_cpu == 0 || max_cpu > TASK_MAX_CPUS)
            max_cpu = TASK_MAX_CPUS;

        for (uint32_t cpu_index = 0; cpu_index < max_cpu; cpu_index++)
        {
            if (!task_cpu_is_online(cpu_index))
                continue;

            uint32_t victim = __atomic_load_n(&task_last_steal_victim[cpu_index], __ATOMIC_RELAXED);
            uint64_t runs = __atomic_load_n(&task_exec_runs[cpu_index], __ATOMIC_RELAXED);
            uint64_t steals = __atomic_load_n(&task_steal_runs[cpu_index], __ATOMIC_RELAXED);
            uint64_t steal_fails = __atomic_load_n(&task_steal_fail_runs[cpu_index], __ATOMIC_RELAXED);
            uint64_t idle_hlt = __atomic_load_n(&task_idle_hlt_runs[cpu_index], __ATOMIC_RELAXED);
            if (victim < TASK_MAX_CPUS)
            {
                uint64_t victim_hits = __atomic_load_n(&task_steal_from_cpu[victim], __ATOMIC_RELAXED);
                kdebug_printf("[SCHED] cpu=%u runs=%llu rq_len=%u steals=%llu steal_fail=%llu victim=%u victim_hits=%llu idle_hlt=%llu\n",
                              cpu_index,
                              (unsigned long long) runs,
                              task_runqueue_depth_cpu(cpu_index),
                              (unsigned long long) steals,
                              (unsigned long long) steal_fails,
                              victim,
                              (unsigned long long) victim_hits,
                              (unsigned long long) idle_hlt);
            }
            else
            {
                kdebug_printf("[SCHED] cpu=%u runs=%llu rq_len=%u steals=%llu steal_fail=%llu victim=none victim_hits=0 idle_hlt=%llu\n",
                              cpu_index,
                              (unsigned long long) runs,
                              task_runqueue_depth_cpu(cpu_index),
                              (unsigned long long) steals,
                              (unsigned long long) steal_fails,
                              (unsigned long long) idle_hlt);
            }
        }

        return;
    }
}

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

static bool task_cpu_is_online(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return false;

    if (!APIC_is_enabled())
        return cpu_index == 0;

    uint8_t apic_id = APIC_get_core_id((uint8_t) cpu_index);
    if (apic_id == 0xFF)
        return false;

    return SMP_is_apic_online(apic_id);
}

static bool task_runqueue_push_cpu(uint32_t cpu_index, task_work_fn_t fn, void* arg, uint32_t affinity_cpu)
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
        slot->affinity_cpu = affinity_cpu;

        rq->tail = (rq->tail + 1U) % TASK_RUNQUEUE_CAPACITY;
        rq->count++;
        queued = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    if (queued)
        task_kick_cpu(cpu_index);

    return queued;
}

static void task_maybe_kick_stealer(uint32_t source_cpu, uint32_t depth)
{
    if (source_cpu >= TASK_MAX_CPUS)
        return;

    if (!APIC_is_enabled() || !task_is_work_stealing_enabled())
        return;

    if (depth < TASK_PUSH_TRIGGER_DEPTH)
        return;

    // Send sparse wakeups only (8, 16, 32, ...) to avoid IPI storms.
    if ((depth & (depth - 1U)) != 0)
        return;

    uint8_t core_count = APIC_get_core_count();
    uint32_t max_cpu = core_count;
    if (max_cpu == 0 || max_cpu > TASK_MAX_CPUS)
        max_cpu = TASK_MAX_CPUS;

    uint32_t cursor = __atomic_load_n(&task_steal_cursor[source_cpu], __ATOMIC_RELAXED);
    if (cursor >= max_cpu)
        cursor = source_cpu % max_cpu;

    for (uint32_t offset = 1; offset <= max_cpu; offset++)
    {
        uint32_t cpu = (cursor + offset) % max_cpu;
        if (cpu == source_cpu || !task_cpu_is_online(cpu))
            continue;

        __atomic_store_n(&task_steal_cursor[source_cpu], cpu, __ATOMIC_RELAXED);
        task_kick_cpu(cpu);
        return;
    }
}

static uint32_t task_pick_push_target(uint32_t source_cpu)
{
    if (source_cpu >= TASK_MAX_CPUS || !task_cpu_is_online(source_cpu))
        return 0;

    if (!task_is_push_balance_enabled())
        return source_cpu;

    uint32_t source_depth = task_runqueue_depth_cpu(source_cpu);
    if (source_depth < TASK_PUSH_TRIGGER_DEPTH)
        return source_cpu;

    uint8_t core_count = APIC_get_core_count();
    uint32_t max_cpu = core_count;
    if (max_cpu == 0 || max_cpu > TASK_MAX_CPUS)
        max_cpu = TASK_MAX_CPUS;

    uint32_t best_cpu = source_cpu;
    uint32_t best_depth = source_depth;
    for (uint32_t cpu = 0; cpu < max_cpu; cpu++)
    {
        if (cpu == source_cpu || !task_cpu_is_online(cpu))
            continue;

        uint32_t depth = task_runqueue_depth_cpu(cpu);
        if (depth < best_depth)
        {
            best_depth = depth;
            best_cpu = cpu;
        }
    }

    if (best_cpu == source_cpu)
        return source_cpu;

    if (source_depth <= (best_depth + TASK_PUSH_IMBALANCE_DELTA))
        return source_cpu;

    return best_cpu;
}

static bool task_runqueue_steal_from_cpu(uint32_t victim_cpu, task_work_item_t* out)
{
    if (!out || victim_cpu >= TASK_MAX_CPUS)
        return false;

    bool has_item = false;
    task_runqueue_t* rq = &task_runqueues[victim_cpu];

    uint64_t flags = spin_lock_irqsave(&rq->lock);
    if (rq->count != 0)
    {
        uint32_t slot = (rq->tail + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
        for (uint32_t scanned = 0; scanned < rq->count; scanned++)
        {
            task_work_item_t* item = &rq->items[slot];
            if (item->affinity_cpu == TASK_WORK_CPU_ANY)
            {
                out->fn = item->fn;
                out->arg = item->arg;
                out->affinity_cpu = TASK_WORK_CPU_ANY;

                uint32_t cursor = slot;
                while (cursor != rq->tail)
                {
                    uint32_t next = (cursor + 1U) % TASK_RUNQUEUE_CAPACITY;
                    if (next == rq->tail)
                        break;

                    rq->items[cursor] = rq->items[next];
                    cursor = next;
                }

                rq->tail = (rq->tail + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
                rq->count--;
                has_item = true;
                break;
            }

            slot = (slot + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
        }
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    return has_item;
}

static bool task_try_steal_work(uint32_t thief_cpu, task_work_item_t* out)
{
    if (!out || thief_cpu >= TASK_MAX_CPUS)
        return false;

    if (!task_is_work_stealing_enabled() || !APIC_is_enabled())
        return false;

    if (SMP_get_online_cpu_count() <= 1)
        return false;

    if (APIC_get_current_lapic_id() == APIC_get_bsp_lapic_id())
        return false;

    uint8_t core_count = APIC_get_core_count();
    uint32_t max_cpu = core_count;
    if (max_cpu == 0 || max_cpu > TASK_MAX_CPUS)
        max_cpu = TASK_MAX_CPUS;

    uint32_t cursor = __atomic_load_n(&task_steal_cursor[thief_cpu], __ATOMIC_RELAXED);
    if (cursor >= max_cpu)
        cursor = thief_cpu % max_cpu;

    uint32_t max_scan = max_cpu;
    if (max_scan > TASK_STEAL_MAX_VICTIMS)
        max_scan = TASK_STEAL_MAX_VICTIMS;

    for (uint32_t offset = 0; offset < max_scan; offset++)
    {
        uint32_t victim_cpu = (cursor + offset) % max_cpu;
        if (victim_cpu == thief_cpu || !task_cpu_is_online(victim_cpu))
            continue;

        if (task_runqueue_steal_from_cpu(victim_cpu, out))
        {
            __atomic_store_n(&task_steal_cursor[thief_cpu], victim_cpu, __ATOMIC_RELAXED);
            __atomic_store_n(&task_last_steal_victim[thief_cpu], victim_cpu, __ATOMIC_RELAXED);
            __atomic_fetch_add(&task_steal_runs[thief_cpu], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&task_steal_from_cpu[victim_cpu], 1, __ATOMIC_RELAXED);
            return true;
        }
    }

    __atomic_fetch_add(&task_steal_fail_runs[thief_cpu], 1, __ATOMIC_RELAXED);
    return false;
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
    memset(task_steal_cursor, 0, sizeof(task_steal_cursor));
    memset(task_exec_runs, 0, sizeof(task_exec_runs));
    __atomic_store_n(&task_exec_runs_total, 0, __ATOMIC_RELAXED);
    memset(task_steal_runs, 0, sizeof(task_steal_runs));
    memset(task_steal_fail_runs, 0, sizeof(task_steal_fail_runs));
    memset(task_steal_from_cpu, 0, sizeof(task_steal_from_cpu));
    memset(task_idle_hlt_runs, 0, sizeof(task_idle_hlt_runs));
    memset(task_last_steal_victim, 0xFF, sizeof(task_last_steal_victim));

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
        task_steal_cursor[cpu] = cpu;
    }
    task_sched_tick_kicks = 0;
    __atomic_store_n(&task_stats_log_epoch, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&task_push_balance_enabled, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&task_work_stealing_enabled, 1, __ATOMIC_RELAXED);

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
    if (!fn)
        return false;

    uint32_t source_cpu = task_current_cpu_index();
    uint32_t target_cpu = task_pick_push_target(source_cpu);
    bool queued = task_runqueue_push_cpu(target_cpu, fn, arg, TASK_WORK_CPU_ANY);
    if (!queued)
        return false;

    if (target_cpu == source_cpu)
        task_maybe_kick_stealer(source_cpu, task_runqueue_depth_cpu(source_cpu));
    return true;
}

bool task_schedule_work_on_cpu(uint32_t cpu_index, task_work_fn_t fn, void* arg)
{
    return task_runqueue_push_cpu(cpu_index, fn, arg, cpu_index);
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
    __atomic_add_fetch(&task_exec_runs[cpu_index], 1, __ATOMIC_RELAXED);
    uint64_t total_runs = __atomic_add_fetch(&task_exec_runs_total, 1, __ATOMIC_RELAXED);
    task_maybe_log_cpu_stats(total_runs);
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

void task_set_push_balance(bool enabled)
{
    __atomic_store_n(&task_push_balance_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

bool task_is_push_balance_enabled(void)
{
    return __atomic_load_n(&task_push_balance_enabled, __ATOMIC_ACQUIRE) != 0;
}

void task_set_work_stealing(bool enabled)
{
    __atomic_store_n(&task_work_stealing_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

bool task_is_work_stealing_enabled(void)
{
    return __atomic_load_n(&task_work_stealing_enabled, __ATOMIC_ACQUIRE) != 0;
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
    uint32_t steal_backoff = TASK_STEAL_BACKOFF_MIN;

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
        {
            steal_backoff = TASK_STEAL_BACKOFF_MIN;
            continue;
        }

        task_work_item_t stolen = { 0 };
        uint32_t current_cpu = task_current_cpu_index();
        if (task_try_steal_work(current_cpu, &stolen))
        {
            stolen.fn(stolen.arg);
            __atomic_add_fetch(&task_exec_runs[current_cpu], 1, __ATOMIC_RELAXED);
            uint64_t total_runs = __atomic_add_fetch(&task_exec_runs_total, 1, __ATOMIC_RELAXED);
            task_maybe_log_cpu_stats(total_runs);
            steal_backoff = TASK_STEAL_BACKOFF_MIN;
            continue;
        }

        for (uint32_t spin = 0; spin < steal_backoff; spin++)
            task_pause();
        if (steal_backoff < TASK_STEAL_BACKOFF_MAX)
            steal_backoff <<= 1U;

        __atomic_fetch_add(&task_idle_hlt_runs[current_cpu], 1, __ATOMIC_RELAXED);
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
