#include <Task/Task.h>

#include <CPU/APIC.h>
#include <CPU/FPU.h>
#include <CPU/GDT.h>
#include <CPU/ISR.h>
#include <CPU/MSR.h>
#include <CPU/SMP.h>
#include <CPU/TSS.h>
#include <Device/HPET.h>
#include <Debug/KDebug.h>
#include <Memory/VMM.h>
#include <Task/RCU.h>

#include <string.h>

// TSS must be aligned on 16 bytes for x86-64.
__attribute__((aligned(16))) static TSS_t tss_per_cpu[TASK_MAX_CPUS];
static task_cpu_local_t task_cpu_local_per_apic[TASK_MAX_CPUS];
static task_runqueue_t task_runqueues[TASK_MAX_CPUS];
static uint32_t task_steal_cursor[TASK_MAX_CPUS];
static uint64_t task_exec_runs[TASK_MAX_CPUS];
static uint64_t task_exec_runs_total = 0;
static uint64_t task_steal_runs[TASK_MAX_CPUS];
static uint64_t task_steal_batch_total[TASK_MAX_CPUS];
static uint64_t task_steal_fail_runs[TASK_MAX_CPUS];
static uint64_t task_steal_from_cpu[TASK_MAX_CPUS];
static uint64_t task_idle_hlt_runs[TASK_MAX_CPUS];
static uint64_t task_rq_lock_contended[TASK_MAX_CPUS];
static uint32_t task_last_steal_victim[TASK_MAX_CPUS];
static uint32_t task_preempt_count[TASK_MAX_CPUS];
static uint8_t task_need_resched[TASK_MAX_CPUS];
static uint8_t task_resched_active[TASK_MAX_CPUS];
static uint64_t task_stats_log_epoch = 0;
static uint8_t task_push_balance_enabled = 1;
static uint8_t task_work_stealing_enabled = 1;
static volatile uint64_t task_sched_tick_kicks = 0;
static task_wait_queue_t task_sleep_waitq;

static task_t kernel_task;

static task_t* current_task;
static task_t* next_task;
static bool task_cpu_is_online(uint32_t cpu_index);

static inline void task_copy_work_item(task_work_item_t* dst, const task_work_item_t* src)
{
    if (!dst || !src)
        return;

    dst->fn = src->fn;
    dst->arg = src->arg;
    dst->affinity_cpu = src->affinity_cpu;
}

static inline void task_pause(void)
{
    __asm__ __volatile__("pause");
}

static inline bool task_interrupts_enabled(void)
{
    uint64_t rflags = 0;
    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(rflags));
    return (rflags & (1ULL << 9)) != 0;
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
            uint64_t steal_batch_total = __atomic_load_n(&task_steal_batch_total[cpu_index], __ATOMIC_RELAXED);
            uint64_t steal_fails = __atomic_load_n(&task_steal_fail_runs[cpu_index], __ATOMIC_RELAXED);
            uint64_t idle_hlt = __atomic_load_n(&task_idle_hlt_runs[cpu_index], __ATOMIC_RELAXED);
            uint64_t lock_contended = __atomic_load_n(&task_rq_lock_contended[cpu_index], __ATOMIC_RELAXED);
            uint64_t kick_recv = SMP_get_sched_kick_count(cpu_index);
            if (victim < TASK_MAX_CPUS)
            {
                uint64_t victim_hits = __atomic_load_n(&task_steal_from_cpu[victim], __ATOMIC_RELAXED);
                kdebug_printf("[SCHED] cpu=%u runs=%llu rq_len=%u steals=%llu steal_batch_total=%llu steal_fail=%llu victim=%u victim_hits=%llu kick_recv=%llu rq_lock_contended=%llu idle_hlt=%llu\n",
                              cpu_index,
                              (unsigned long long) runs,
                              task_runqueue_depth_cpu(cpu_index),
                              (unsigned long long) steals,
                              (unsigned long long) steal_batch_total,
                              (unsigned long long) steal_fails,
                              victim,
                              (unsigned long long) victim_hits,
                              (unsigned long long) kick_recv,
                              (unsigned long long) lock_contended,
                              (unsigned long long) idle_hlt);
            }
            else
            {
                kdebug_printf("[SCHED] cpu=%u runs=%llu rq_len=%u steals=%llu steal_batch_total=%llu steal_fail=%llu victim=none victim_hits=0 kick_recv=%llu rq_lock_contended=%llu idle_hlt=%llu\n",
                              cpu_index,
                              (unsigned long long) runs,
                              task_runqueue_depth_cpu(cpu_index),
                              (unsigned long long) steals,
                              (unsigned long long) steal_batch_total,
                              (unsigned long long) steal_fails,
                              (unsigned long long) kick_recv,
                              (unsigned long long) lock_contended,
                              (unsigned long long) idle_hlt);
            }
        }

        return;
    }
}

static void task_try_resched_now(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    if (__atomic_load_n(&task_preempt_count[cpu_index], __ATOMIC_ACQUIRE) != 0)
        return;

    if (__atomic_load_n(&task_need_resched[cpu_index], __ATOMIC_ACQUIRE) == 0)
        return;

    if (__atomic_exchange_n(&task_resched_active[cpu_index], 1, __ATOMIC_ACQ_REL) != 0)
        return;

    if (__atomic_exchange_n(&task_need_resched[cpu_index], 0, __ATOMIC_ACQ_REL) != 0)
        (void) task_run_next_work_on_cpu(cpu_index);

    if (__atomic_load_n(&task_runqueues[cpu_index].count, __ATOMIC_RELAXED) != 0)
        __atomic_store_n(&task_need_resched[cpu_index], 1, __ATOMIC_RELEASE);

    __atomic_store_n(&task_resched_active[cpu_index], 0, __ATOMIC_RELEASE);
}

static bool task_cpu_local_is_valid(const task_cpu_local_t* cpu_local)
{
    if (!cpu_local)
        return false;

    if (__atomic_load_n(&cpu_local->magic, __ATOMIC_ACQUIRE) != TASK_CPU_LOCAL_MAGIC)
        return false;

    uint32_t cpu_index = __atomic_load_n(&cpu_local->cpu_index, __ATOMIC_RELAXED);
    uint8_t apic_id = __atomic_load_n(&cpu_local->apic_id, __ATOMIC_RELAXED);
    return cpu_index < TASK_MAX_CPUS && apic_id < TASK_MAX_CPUS;
}

task_cpu_local_t* task_get_cpu_local(void)
{
    uint64_t kernel_gs_base = MSR_get(IA32_KERNEL_GS_BASE);
    if (kernel_gs_base != 0)
    {
        task_cpu_local_t* cpu_local = (task_cpu_local_t*) (uintptr_t) kernel_gs_base;
        if (task_cpu_local_is_valid(cpu_local))
            return cpu_local;
    }

    if (APIC_is_enabled())
    {
        uint8_t apic_id = APIC_get_current_lapic_id();
        if (apic_id < TASK_MAX_CPUS)
        {
            task_cpu_local_t* cpu_local = &task_cpu_local_per_apic[apic_id];
            if (task_cpu_local_is_valid(cpu_local))
                return cpu_local;
        }
    }

    return NULL;
}

static uint32_t task_current_cpu_index(void)
{
    task_cpu_local_t* cpu_local = task_get_cpu_local();
    if (cpu_local)
        return __atomic_load_n(&cpu_local->cpu_index, __ATOMIC_RELAXED);

    if (current_task == NULL)
        return 0;

    uint32_t smp_cpu_index = SMP_get_current_cpu();
    if (smp_cpu_index >= TASK_MAX_CPUS)
        return 0;
    return smp_cpu_index;
}

uint32_t task_get_current_cpu_index(void)
{
    return task_current_cpu_index();
}

task_t* task_get_current_task(void)
{
    return current_task;
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

void task_wait_queue_init(task_wait_queue_t* queue)
{
    if (!queue)
        return;

    spinlock_init(&queue->lock);
    queue->head = NULL;
    queue->tail = NULL;
    __atomic_store_n(&queue->waiters, 0, __ATOMIC_RELAXED);
}

void task_waiter_init(task_waiter_t* waiter)
{
    if (!waiter)
        return;

    memset(waiter, 0, sizeof(*waiter));
    waiter->cpu_index = task_current_cpu_index();
}

static void task_waiter_signal(task_waiter_t* waiter, bool kick_remote)
{
    if (!waiter)
        return;

    if (__atomic_exchange_n(&waiter->signaled, 1, __ATOMIC_ACQ_REL) != 0)
        return;

    uint32_t target_cpu = __atomic_load_n(&waiter->cpu_index, __ATOMIC_ACQUIRE);
    if (kick_remote && target_cpu < TASK_MAX_CPUS && target_cpu != task_current_cpu_index())
        task_kick_cpu(target_cpu);
}

static void task_wait_queue_enqueue_locked(task_wait_queue_t* queue, task_waiter_t* waiter)
{
    if (!queue || !waiter)
        return;

    waiter->next = NULL;
    __atomic_store_n(&waiter->queued, 1, __ATOMIC_RELEASE);

    if (queue->tail)
        queue->tail->next = waiter;
    else
        queue->head = waiter;

    queue->tail = waiter;
    __atomic_fetch_add(&queue->waiters, 1, __ATOMIC_RELAXED);
}

static bool task_wait_queue_cancel_locked(task_wait_queue_t* queue, task_waiter_t* waiter)
{
    if (!queue || !waiter)
        return false;

    task_waiter_t* prev = NULL;
    task_waiter_t* cur = queue->head;
    while (cur)
    {
        if (cur == waiter)
        {
            if (prev)
                prev->next = cur->next;
            else
                queue->head = cur->next;

            if (queue->tail == cur)
                queue->tail = prev;

            cur->next = NULL;
            __atomic_store_n(&cur->queued, 0, __ATOMIC_RELEASE);

            if (__atomic_load_n(&queue->waiters, __ATOMIC_RELAXED) != 0)
                __atomic_fetch_sub(&queue->waiters, 1, __ATOMIC_RELAXED);

            return true;
        }

        prev = cur;
        cur = cur->next;
    }

    return false;
}

static void task_wait_queue_cancel(task_wait_queue_t* queue, task_waiter_t* waiter)
{
    if (!queue || !waiter)
        return;

    uint64_t flags = spin_lock_irqsave(&queue->lock);
    (void) task_wait_queue_cancel_locked(queue, waiter);
    spin_unlock_irqrestore(&queue->lock, flags);
}

static bool task_waiter_wait(task_waiter_t* waiter, uint64_t timeout_ticks)
{
    if (!waiter)
        return false;

    if (__atomic_load_n(&waiter->signaled, __ATOMIC_ACQUIRE) != 0)
        return true;

    if (!task_interrupts_enabled())
        return false;

    uint64_t start_ticks = 0;
    if (timeout_ticks != TASK_WAIT_TIMEOUT_INFINITE)
        start_ticks = ISR_get_timer_ticks();

    while (__atomic_load_n(&waiter->signaled, __ATOMIC_ACQUIRE) == 0)
    {
        if (timeout_ticks != TASK_WAIT_TIMEOUT_INFINITE)
        {
            uint64_t now_ticks = ISR_get_timer_ticks();
            if ((now_ticks - start_ticks) >= timeout_ticks)
                return false;
        }

        __asm__ __volatile__("hlt");
    }

    return true;
}

uint64_t task_ticks_from_ms(uint32_t ms)
{
    if (ms == 0)
        return 0;

    uint32_t tick_hz = ISR_get_tick_hz();
    if (tick_hz == 0)
        tick_hz = 100;

    uint64_t ticks = (((uint64_t) ms * tick_hz) + 999ULL) / 1000ULL;
    if (ticks == 0)
        ticks = 1;

    return ticks;
}

bool task_wait_queue_wait_event(task_wait_queue_t* queue,
                                task_waiter_t* waiter,
                                task_wait_predicate_t predicate,
                                void* context,
                                uint64_t timeout_ticks)
{
    if (!queue || !waiter || !predicate)
        return false;

    if (!predicate(context))
        return true;

    if (!task_interrupts_enabled())
        return false;

    uint64_t start_ticks = ISR_get_timer_ticks();

    while (predicate(context))
    {
        task_waiter_init(waiter);

        uint64_t flags = spin_lock_irqsave(&queue->lock);
        task_wait_queue_enqueue_locked(queue, waiter);
        spin_unlock_irqrestore(&queue->lock, flags);

        if (!predicate(context))
        {
            task_wait_queue_cancel(queue, waiter);
            return true;
        }

        uint64_t remaining = timeout_ticks;
        if (timeout_ticks != TASK_WAIT_TIMEOUT_INFINITE)
        {
            uint64_t elapsed = ISR_get_timer_ticks() - start_ticks;
            if (elapsed >= timeout_ticks)
            {
                task_wait_queue_cancel(queue, waiter);
                return !predicate(context);
            }

            remaining = timeout_ticks - elapsed;
        }

        if (!task_waiter_wait(waiter, remaining))
        {
            task_wait_queue_cancel(queue, waiter);
            if (timeout_ticks != TASK_WAIT_TIMEOUT_INFINITE)
            {
                uint64_t elapsed = ISR_get_timer_ticks() - start_ticks;
                if (elapsed >= timeout_ticks)
                    return !predicate(context);
            }
        }
    }

    return true;
}

void task_wait_queue_wake_one(task_wait_queue_t* queue)
{
    if (!queue)
        return;

    task_waiter_t* waiter = NULL;

    uint64_t flags = spin_lock_irqsave(&queue->lock);
    if (queue->head)
    {
        waiter = queue->head;
        queue->head = waiter->next;
        if (queue->head == NULL)
            queue->tail = NULL;

        waiter->next = NULL;
        __atomic_store_n(&waiter->queued, 0, __ATOMIC_RELEASE);
        if (__atomic_load_n(&queue->waiters, __ATOMIC_RELAXED) != 0)
            __atomic_fetch_sub(&queue->waiters, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&queue->lock, flags);

    task_waiter_signal(waiter, true);
}

static void task_wait_queue_wake_all_internal(task_wait_queue_t* queue, bool kick_remote)
{
    if (!queue)
        return;

    task_waiter_t* waiters = NULL;

    uint64_t flags = spin_lock_irqsave(&queue->lock);
    waiters = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    __atomic_store_n(&queue->waiters, 0, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&queue->lock, flags);

    while (waiters)
    {
        task_waiter_t* next = waiters->next;
        waiters->next = NULL;
        __atomic_store_n(&waiters->queued, 0, __ATOMIC_RELEASE);
        task_waiter_signal(waiters, kick_remote);
        waiters = next;
    }
}

void task_wait_queue_wake_all(task_wait_queue_t* queue)
{
    task_wait_queue_wake_all_internal(queue, true);
}

static bool task_sleep_wait_predicate(void* context)
{
    task_sleep_wait_context_t* sleep_ctx = (task_sleep_wait_context_t*) context;
    if (!sleep_ctx)
        return false;

    if (sleep_ctx->use_hpet)
        return HPET_get_counter() < sleep_ctx->deadline;

    return ISR_get_timer_ticks() < sleep_ctx->deadline;
}

bool task_sleep_ms(uint32_t ms)
{
    if (ms == 0)
        return true;

    task_sleep_wait_context_t sleep_ctx = { 0 };
    if (HPET_is_available() && HPET_get_frequency_hz() != 0)
    {
        uint64_t hpet_hz = HPET_get_frequency_hz();
        uint64_t delta = (((uint64_t) ms * hpet_hz) + 999ULL) / 1000ULL;
        if (delta == 0)
            delta = 1;

        uint64_t now = HPET_get_counter();
        sleep_ctx.deadline = (TASK_WAIT_TIMEOUT_INFINITE - now < delta) ? TASK_WAIT_TIMEOUT_INFINITE : (now + delta);
        sleep_ctx.use_hpet = 1;
    }
    else
    {
        uint64_t delta = task_ticks_from_ms(ms);
        uint64_t now = ISR_get_timer_ticks();
        sleep_ctx.deadline = (TASK_WAIT_TIMEOUT_INFINITE - now < delta) ? TASK_WAIT_TIMEOUT_INFINITE : (now + delta);
        sleep_ctx.use_hpet = 0;
    }

    task_waiter_t waiter = { 0 };
    return task_wait_queue_wait_event(&task_sleep_waitq,
                                      &waiter,
                                      task_sleep_wait_predicate,
                                      &sleep_ctx,
                                      TASK_WAIT_TIMEOUT_INFINITE);
}

static inline uint64_t task_runqueue_lock(uint32_t cpu_index, task_runqueue_t* rq)
{
    if (cpu_index < TASK_MAX_CPUS &&
        __atomic_load_n(&rq->lock.locked, __ATOMIC_RELAXED) != 0)
    {
        __atomic_fetch_add(&task_rq_lock_contended[cpu_index], 1, __ATOMIC_RELAXED);
    }

    return spin_lock_irqsave(&rq->lock);
}

static bool task_runqueue_enqueue_item(uint32_t cpu_index, const task_work_item_t* item, bool kick_cpu)
{
    if (!item || !item->fn || cpu_index >= TASK_MAX_CPUS)
        return false;

    bool queued = false;
    task_runqueue_t* rq = &task_runqueues[cpu_index];

    uint64_t flags = task_runqueue_lock(cpu_index, rq);
    if (rq->count < TASK_RUNQUEUE_CAPACITY)
    {
        task_copy_work_item(&rq->items[rq->tail], item);
        rq->tail = (rq->tail + 1U) % TASK_RUNQUEUE_CAPACITY;
        rq->count++;
        queued = true;
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    if (queued && kick_cpu)
        task_kick_cpu(cpu_index);

    return queued;
}

static bool task_runqueue_push_cpu(uint32_t cpu_index, task_work_fn_t fn, void* arg, uint32_t affinity_cpu)
{
    if (!fn || cpu_index >= TASK_MAX_CPUS)
        return false;

    task_work_item_t item = {
        .fn = fn,
        .arg = arg,
        .affinity_cpu = affinity_cpu
    };

    return task_runqueue_enqueue_item(cpu_index, &item, true);
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
        cursor = 0;

    for (uint32_t probe = 0; probe < max_cpu; probe++)
    {
        uint32_t cpu = (source_cpu + 1U + cursor) % max_cpu;
        cursor = (cursor + 1U) % max_cpu;

        if (cpu == source_cpu || !task_cpu_is_online(cpu))
            continue;

        __atomic_store_n(&task_steal_cursor[source_cpu], cursor, __ATOMIC_RELAXED);
        task_kick_cpu(cpu);
        return;
    }

    __atomic_store_n(&task_steal_cursor[source_cpu], cursor, __ATOMIC_RELAXED);
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

static uint32_t task_runqueue_steal_batch_from_cpu(uint32_t victim_cpu, task_work_item_t* out, uint32_t max_items)
{
    if (!out || victim_cpu >= TASK_MAX_CPUS || max_items == 0)
        return 0;

    uint32_t stolen = 0;
    task_runqueue_t* rq = &task_runqueues[victim_cpu];

    uint64_t flags = task_runqueue_lock(victim_cpu, rq);
    while (rq->count != 0 && stolen < max_items)
    {
        bool found = false;
        uint32_t snapshot_count = rq->count;
        uint32_t slot = (rq->tail + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
        for (uint32_t scanned = 0; scanned < snapshot_count; scanned++)
        {
            task_work_item_t* item = &rq->items[slot];
            if (item->affinity_cpu == TASK_WORK_CPU_ANY)
            {
                task_copy_work_item(&out[stolen], item);
                stolen++;

                uint32_t cursor = slot;
                while (cursor != rq->tail)
                {
                    uint32_t next = (cursor + 1U) % TASK_RUNQUEUE_CAPACITY;
                    if (next == rq->tail)
                        break;

                    task_copy_work_item(&rq->items[cursor], &rq->items[next]);
                    cursor = next;
                }

                rq->tail = (rq->tail + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
                rq->count--;
                found = true;
                break;
            }

            slot = (slot + TASK_RUNQUEUE_CAPACITY - 1U) % TASK_RUNQUEUE_CAPACITY;
        }

        if (!found)
            break;
    }
    spin_unlock_irqrestore(&rq->lock, flags);

    return stolen;
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
        cursor = 0;

    uint32_t max_scan = max_cpu - 1U;
    if (max_scan > TASK_STEAL_MAX_VICTIMS)
        max_scan = TASK_STEAL_MAX_VICTIMS;

    uint32_t scanned_victims = 0;
    for (uint32_t probe = 0; probe < max_cpu && scanned_victims < max_scan; probe++)
    {
        uint32_t victim_cpu = (thief_cpu + 1U + cursor) % max_cpu;
        cursor = (cursor + 1U) % max_cpu;

        if (victim_cpu == thief_cpu || !task_cpu_is_online(victim_cpu))
            continue;

        scanned_victims++;

        uint32_t batch_max = 1;
        uint32_t victim_depth = task_runqueue_depth_cpu(victim_cpu);
        if (victim_depth > TASK_STEAL_BATCH_TRIGGER_DEPTH)
            batch_max = TASK_STEAL_BATCH_MAX;

        uint32_t local_depth = task_runqueue_depth_cpu(thief_cpu);
        uint32_t local_free = (local_depth < TASK_RUNQUEUE_CAPACITY) ? (TASK_RUNQUEUE_CAPACITY - local_depth) : 0;
        if (batch_max > (local_free + 1U))
            batch_max = local_free + 1U;
        if (batch_max == 0)
            batch_max = 1;

        task_work_item_t stolen_items[TASK_STEAL_BATCH_MAX] = { 0 };
        uint32_t stolen_count = task_runqueue_steal_batch_from_cpu(victim_cpu, stolen_items, batch_max);
        if (stolen_count != 0)
        {
            task_copy_work_item(out, &stolen_items[0]);

            for (uint32_t i = 1; i < stolen_count; i++)
            {
                if (!task_runqueue_enqueue_item(thief_cpu, &stolen_items[i], false))
                {
                    stolen_items[i].fn(stolen_items[i].arg);
                    __atomic_add_fetch(&task_exec_runs[thief_cpu], 1, __ATOMIC_RELAXED);
                    uint64_t total_runs = __atomic_add_fetch(&task_exec_runs_total, 1, __ATOMIC_RELAXED);
                    task_maybe_log_cpu_stats(total_runs);
                }
            }

            __atomic_store_n(&task_last_steal_victim[thief_cpu], victim_cpu, __ATOMIC_RELAXED);
            __atomic_fetch_add(&task_steal_runs[thief_cpu], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&task_steal_batch_total[thief_cpu], stolen_count, __ATOMIC_RELAXED);
            __atomic_fetch_add(&task_steal_from_cpu[victim_cpu], 1, __ATOMIC_RELAXED);
            __atomic_store_n(&task_steal_cursor[thief_cpu], cursor, __ATOMIC_RELAXED);
            return true;
        }
    }

    __atomic_store_n(&task_steal_cursor[thief_cpu], cursor, __ATOMIC_RELAXED);
    __atomic_fetch_add(&task_steal_fail_runs[thief_cpu], 1, __ATOMIC_RELAXED);
    return false;
}

static bool task_runqueue_pop_cpu(uint32_t cpu_index, task_work_item_t* out)
{
    if (!out || cpu_index >= TASK_MAX_CPUS)
        return false;

    bool has_item = false;
    task_runqueue_t* rq = &task_runqueues[cpu_index];

    uint64_t flags = task_runqueue_lock(cpu_index, rq);
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
    memset(task_steal_batch_total, 0, sizeof(task_steal_batch_total));
    memset(task_steal_fail_runs, 0, sizeof(task_steal_fail_runs));
    memset(task_steal_from_cpu, 0, sizeof(task_steal_from_cpu));
    memset(task_idle_hlt_runs, 0, sizeof(task_idle_hlt_runs));
    memset(task_rq_lock_contended, 0, sizeof(task_rq_lock_contended));
    memset(task_last_steal_victim, 0xFF, sizeof(task_last_steal_victim));
    memset(task_preempt_count, 0, sizeof(task_preempt_count));
    memset(task_need_resched, 0, sizeof(task_need_resched));
    memset(task_resched_active, 0, sizeof(task_resched_active));

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
        task_steal_cursor[cpu] = 0;
    }
    task_sched_tick_kicks = 0;
    __atomic_store_n(&task_stats_log_epoch, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&task_push_balance_enabled, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&task_work_stealing_enabled, 1, __ATOMIC_RELAXED);
    task_wait_queue_init(&task_sleep_waitq);
    RCU_init();

    (void) task_init_cpu(0, kernel_stack, APIC_get_bsp_lapic_id());
}

bool task_init_cpu(uint32_t cpu_index, uintptr_t kernel_stack, uint8_t apic_id)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return false;

    VMM_enable_nx_current_cpu();

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
        memset(cpu_local, 0, sizeof(*cpu_local));
        cpu_local->syscall_rsp0 = tss->rsp0;
        cpu_local->cpu_index = cpu_index;
        cpu_local->apic_id = apic_id;
        cpu_local->online = 1;
        cpu_local->magic = TASK_CPU_LOCAL_MAGIC;
        cpu_local->syscall_count = 0;
        cpu_local->current_task = (uintptr_t) current_task;
        cpu_local->syscall_user_rsp = 0;

        // SYSCALL stub uses SWAPGS + GS:[TASK_CPU_LOCAL_SYSCALL_RSP0_OFF].
        MSR_set(IA32_GS_BASE, 0);
        MSR_set(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) cpu_local);
    }

    // Lazy FPU: force #NM on the first SIMD/FPU instruction after a context bind.
    FPU_lazy_on_task_switch();
    FPU_lazy_probe_current_cpu();

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
    // Sleep waiters are driven by periodic timer IRQs (LAPIC or PIT fallback).
    task_wait_queue_wake_all_internal(&task_sleep_waitq, false);
    RCU_note_quiescent_state();

    if (!APIC_is_enabled())
        return;

    uint32_t cpu_index = task_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    if (__atomic_load_n(&task_runqueues[cpu_index].count, __ATOMIC_RELAXED) == 0)
        return;

    __atomic_store_n(&task_need_resched[cpu_index], 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&task_sched_tick_kicks, 1, __ATOMIC_RELAXED);
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

void task_preempt_disable(void)
{
    uint32_t cpu_index = task_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    __atomic_add_fetch(&task_preempt_count[cpu_index], 1, __ATOMIC_ACQ_REL);
}

void task_preempt_enable(void)
{
    uint32_t cpu_index = task_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    uint32_t previous = __atomic_load_n(&task_preempt_count[cpu_index], __ATOMIC_RELAXED);
    if (previous == 0)
        return;

    uint32_t current = __atomic_sub_fetch(&task_preempt_count[cpu_index], 1, __ATOMIC_ACQ_REL);
    if (current == 0)
    {
        if (task_interrupts_enabled())
            task_try_resched_now(cpu_index);
    }
}

uint32_t task_get_preempt_count(void)
{
    return task_get_preempt_count_cpu(task_current_cpu_index());
}

uint32_t task_get_preempt_count_cpu(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return 0;

    return __atomic_load_n(&task_preempt_count[cpu_index], __ATOMIC_ACQUIRE);
}

void task_irq_exit(void)
{
    uint32_t cpu_index = task_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    task_try_resched_now(cpu_index);
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

    // Lazy FPU: defer save/restore until first SIMD/FPU instruction of the new task.
    FPU_lazy_on_task_switch();

    // Update rsp0 for new task's kernel stack
    // rsp1 and rsp2 are not used for IRQ handling
    uint32_t cpu_index = SMP_get_current_cpu();
    if (cpu_index >= TASK_MAX_CPUS)
        cpu_index = 0;
    TSS_t* cpu_tss = &tss_per_cpu[cpu_index];
    cpu_tss->rsp0 = (current_task->stack & ~0xF);

    task_cpu_local_t* cpu_local = task_get_cpu_local();
    if (cpu_local)
    {
        cpu_local->syscall_rsp0 = cpu_tss->rsp0;
        cpu_local->current_task = (uintptr_t) current_task;
        cpu_local->syscall_user_rsp = 0;
    }
    
    // IMPORTANT: After modifying TSS, we must reload it in TR
    // The CPU caches TSS data, so changes require a reload
    GDT_load_TSS_segment(cpu_tss);
    TSS_flush(TSS_SYSTEM_SEGMENT);
    
    // Jump (not call) to avoid leaving a return address on the stack.
    // A stray return address corrupts the interrupt frame and breaks iretq.
    __asm__ __volatile__("jmp perform_task_switch" : : "D"(current_task) : "memory");
    __builtin_unreachable();
}
