#include <Task/RCU.h>

#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/SMP.h>
#include <Debug/Spinlock.h>
#include <Memory/KMem.h>
#include <Task/Task.h>

_Static_assert(RCU_CPU_SLOTS == TASK_MAX_CPUS,
               "RCU layout mismatch: CPU slot count must match scheduler CPU count");

static rcu_runtime_state_t rcu_state;

static bool rcu_cpu_online(uint32_t cpu_index)
{
    if (cpu_index >= RCU_CPU_SLOTS)
        return false;

    if (!APIC_is_enabled())
        return cpu_index == 0;

    uint8_t apic_id = APIC_get_core_id((uint8_t) cpu_index);
    if (apic_id == 0xFF)
        return false;

    return SMP_is_apic_online(apic_id);
}

static uint32_t rcu_online_cpu_limit(void)
{
    uint32_t max_cpu = APIC_get_core_count();
    if (max_cpu == 0 || max_cpu > RCU_CPU_SLOTS)
        max_cpu = RCU_CPU_SLOTS;
    return max_cpu;
}

static bool rcu_can_report_qs(uint32_t cpu_index)
{
    if (cpu_index >= RCU_CPU_SLOTS)
        return false;

    if (__atomic_load_n(&rcu_state.cpu[cpu_index].read_depth, __ATOMIC_ACQUIRE) != 0)
        return false;

    if (task_get_preempt_count_cpu(cpu_index) != 0)
        return false;

    return true;
}

static uint64_t rcu_start_gp_locked(void)
{
    if (rcu_state.gp_target != 0)
        return rcu_state.gp_target;

    uint64_t target = rcu_state.gp_seq + 1;
    rcu_state.gp_target = target;

    uint32_t max_cpu = rcu_online_cpu_limit();
    for (uint32_t cpu = 0; cpu < max_cpu; cpu++)
    {
        if (!rcu_cpu_online(cpu))
            continue;

        if (rcu_can_report_qs(cpu))
            rcu_state.cpu[cpu].seen_gp = target;
    }

    return target;
}

static uint64_t rcu_try_complete_gp_locked(void)
{
    if (rcu_state.gp_target == 0)
        return 0;

    uint64_t target = rcu_state.gp_target;
    uint32_t max_cpu = rcu_online_cpu_limit();

    for (uint32_t cpu = 0; cpu < max_cpu; cpu++)
    {
        if (!rcu_cpu_online(cpu))
            continue;

        if (rcu_state.cpu[cpu].seen_gp < target)
            return 0;
    }

    rcu_state.gp_seq = target;
    rcu_state.gp_target = 0;
    return target;
}

static rcu_callback_node_t* rcu_detach_ready_callbacks_locked(uint64_t completed_gp)
{
    if (completed_gp == 0 || !rcu_state.cb_head)
        return NULL;

    rcu_callback_node_t* ready_head = NULL;
    rcu_callback_node_t* ready_tail = NULL;

    while (rcu_state.cb_head && rcu_state.cb_head->target_gp <= completed_gp)
    {
        rcu_callback_node_t* node = rcu_state.cb_head;
        rcu_state.cb_head = node->next;
        node->next = NULL;

        if (!ready_head)
            ready_head = node;
        else
            ready_tail->next = node;

        ready_tail = node;
        if (rcu_state.cb_pending != 0)
            rcu_state.cb_pending--;
    }

    if (!rcu_state.cb_head)
        rcu_state.cb_tail = NULL;

    return ready_head;
}

static void rcu_run_callback_list(rcu_callback_node_t* list)
{
    while (list)
    {
        rcu_callback_node_t* next = list->next;
        if (list->fn)
            list->fn(list->context);
        kfree(list);
        list = next;
    }
}

void RCU_init(void)
{
    spinlock_init(&rcu_state.lock);
    for (uint32_t cpu = 0; cpu < RCU_CPU_SLOTS; cpu++)
    {
        rcu_state.cpu[cpu].read_depth = 0;
        rcu_state.cpu[cpu].seen_gp = 0;
    }

    rcu_state.gp_seq = 0;
    rcu_state.gp_target = 0;
    rcu_state.cb_head = NULL;
    rcu_state.cb_tail = NULL;
    rcu_state.cb_pending = 0;
    rcu_state.ready = true;
}

void RCU_note_quiescent_state(void)
{
    if (!rcu_state.ready)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= RCU_CPU_SLOTS)
        return;

    if (!rcu_can_report_qs(cpu_index))
        return;

    uint64_t completed_gp = 0;
    rcu_callback_node_t* ready = NULL;

    uint64_t flags = spin_lock_irqsave(&rcu_state.lock);
    if (rcu_state.gp_target != 0 && rcu_state.cpu[cpu_index].seen_gp < rcu_state.gp_target)
        rcu_state.cpu[cpu_index].seen_gp = rcu_state.gp_target;

    completed_gp = rcu_try_complete_gp_locked();
    ready = rcu_detach_ready_callbacks_locked(completed_gp);
    spin_unlock_irqrestore(&rcu_state.lock, flags);

    if (ready)
        rcu_run_callback_list(ready);
}

void RCU_read_lock(void)
{
    if (!rcu_state.ready)
        return;

    task_preempt_disable();

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= RCU_CPU_SLOTS)
        return;

    __atomic_add_fetch(&rcu_state.cpu[cpu_index].read_depth, 1, __ATOMIC_ACQ_REL);
}

void RCU_read_unlock(void)
{
    if (!rcu_state.ready)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index < RCU_CPU_SLOTS)
    {
        uint32_t depth = __atomic_load_n(&rcu_state.cpu[cpu_index].read_depth, __ATOMIC_RELAXED);
        if (depth != 0)
            __atomic_sub_fetch(&rcu_state.cpu[cpu_index].read_depth, 1, __ATOMIC_ACQ_REL);
    }

    task_preempt_enable();
}

bool RCU_call(rcu_callback_fn_t fn, void* context)
{
    if (!rcu_state.ready || !fn)
        return false;

    rcu_callback_node_t* node = (rcu_callback_node_t*) kmalloc(sizeof(*node));
    if (!node)
        return false;

    node->next = NULL;
    node->fn = fn;
    node->context = context;
    node->target_gp = 0;

    uint64_t flags = spin_lock_irqsave(&rcu_state.lock);
    uint64_t target = rcu_start_gp_locked();
    node->target_gp = target;

    if (rcu_state.cb_tail)
        rcu_state.cb_tail->next = node;
    else
        rcu_state.cb_head = node;

    rcu_state.cb_tail = node;
    rcu_state.cb_pending++;
    spin_unlock_irqrestore(&rcu_state.lock, flags);

    RCU_note_quiescent_state();
    return true;
}

bool RCU_synchronize(void)
{
    if (!rcu_state.ready)
        return false;

    uint64_t target = 0;
    uint64_t flags = spin_lock_irqsave(&rcu_state.lock);
    target = rcu_start_gp_locked();
    spin_unlock_irqrestore(&rcu_state.lock, flags);

    if (target == 0)
        return true;

    uint64_t start_ticks = ISR_get_timer_ticks();
    while (true)
    {
        uint64_t observed = __atomic_load_n(&rcu_state.gp_seq, __ATOMIC_ACQUIRE);
        if (observed >= target)
            return true;

        RCU_note_quiescent_state();
        __asm__ __volatile__("pause");

        if ((ISR_get_timer_ticks() - start_ticks) > RCU_SYNC_TIMEOUT_TICKS)
            return false;
    }
}

void RCU_get_stats(rcu_stats_t* out_stats)
{
    if (!out_stats)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= RCU_CPU_SLOTS)
        cpu_index = 0;

    uint64_t flags = spin_lock_irqsave(&rcu_state.lock);
    out_stats->gp_seq = rcu_state.gp_seq;
    out_stats->gp_target = rcu_state.gp_target;
    out_stats->callbacks_pending = rcu_state.cb_pending;
    out_stats->local_read_depth = __atomic_load_n(&rcu_state.cpu[cpu_index].read_depth, __ATOMIC_RELAXED);
    out_stats->local_preempt_count = task_get_preempt_count_cpu(cpu_index);
    spin_unlock_irqrestore(&rcu_state.lock, flags);
}
