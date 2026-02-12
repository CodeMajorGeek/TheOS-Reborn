#include <Task/RCU.h>

#include <CPU/APIC.h>
#include <CPU/ISR.h>
#include <CPU/SMP.h>
#include <Debug/Spinlock.h>
#include <Memory/KMem.h>
#include <Task/Task.h>

static spinlock_t rcu_lock;
static rcu_cpu_state_t rcu_cpu[TASK_MAX_CPUS];
static uint64_t rcu_gp_seq = 0;
static uint64_t rcu_gp_target = 0;
static rcu_callback_node_t* rcu_cb_head = NULL;
static rcu_callback_node_t* rcu_cb_tail = NULL;
static uint64_t rcu_cb_pending = 0;
static bool rcu_ready = false;

static bool rcu_cpu_online(uint32_t cpu_index)
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

static uint32_t rcu_online_cpu_limit(void)
{
    uint32_t max_cpu = APIC_get_core_count();
    if (max_cpu == 0 || max_cpu > TASK_MAX_CPUS)
        max_cpu = TASK_MAX_CPUS;
    return max_cpu;
}

static bool rcu_can_report_qs(uint32_t cpu_index)
{
    if (cpu_index >= TASK_MAX_CPUS)
        return false;

    if (__atomic_load_n(&rcu_cpu[cpu_index].read_depth, __ATOMIC_ACQUIRE) != 0)
        return false;

    if (task_get_preempt_count_cpu(cpu_index) != 0)
        return false;

    return true;
}

static uint64_t rcu_start_gp_locked(void)
{
    if (rcu_gp_target != 0)
        return rcu_gp_target;

    uint64_t target = rcu_gp_seq + 1;
    rcu_gp_target = target;

    uint32_t max_cpu = rcu_online_cpu_limit();
    for (uint32_t cpu = 0; cpu < max_cpu; cpu++)
    {
        if (!rcu_cpu_online(cpu))
            continue;

        if (rcu_can_report_qs(cpu))
            rcu_cpu[cpu].seen_gp = target;
    }

    return target;
}

static uint64_t rcu_try_complete_gp_locked(void)
{
    if (rcu_gp_target == 0)
        return 0;

    uint64_t target = rcu_gp_target;
    uint32_t max_cpu = rcu_online_cpu_limit();

    for (uint32_t cpu = 0; cpu < max_cpu; cpu++)
    {
        if (!rcu_cpu_online(cpu))
            continue;

        if (rcu_cpu[cpu].seen_gp < target)
            return 0;
    }

    rcu_gp_seq = target;
    rcu_gp_target = 0;
    return target;
}

static rcu_callback_node_t* rcu_detach_ready_callbacks_locked(uint64_t completed_gp)
{
    if (completed_gp == 0 || !rcu_cb_head)
        return NULL;

    rcu_callback_node_t* ready_head = NULL;
    rcu_callback_node_t* ready_tail = NULL;

    while (rcu_cb_head && rcu_cb_head->target_gp <= completed_gp)
    {
        rcu_callback_node_t* node = rcu_cb_head;
        rcu_cb_head = node->next;
        node->next = NULL;

        if (!ready_head)
            ready_head = node;
        else
            ready_tail->next = node;

        ready_tail = node;
        if (rcu_cb_pending != 0)
            rcu_cb_pending--;
    }

    if (!rcu_cb_head)
        rcu_cb_tail = NULL;

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
    spinlock_init(&rcu_lock);
    for (uint32_t cpu = 0; cpu < TASK_MAX_CPUS; cpu++)
    {
        rcu_cpu[cpu].read_depth = 0;
        rcu_cpu[cpu].seen_gp = 0;
    }

    rcu_gp_seq = 0;
    rcu_gp_target = 0;
    rcu_cb_head = NULL;
    rcu_cb_tail = NULL;
    rcu_cb_pending = 0;
    rcu_ready = true;
}

void RCU_note_quiescent_state(void)
{
    if (!rcu_ready)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    if (!rcu_can_report_qs(cpu_index))
        return;

    uint64_t completed_gp = 0;
    rcu_callback_node_t* ready = NULL;

    uint64_t flags = spin_lock_irqsave(&rcu_lock);
    if (rcu_gp_target != 0 && rcu_cpu[cpu_index].seen_gp < rcu_gp_target)
        rcu_cpu[cpu_index].seen_gp = rcu_gp_target;

    completed_gp = rcu_try_complete_gp_locked();
    ready = rcu_detach_ready_callbacks_locked(completed_gp);
    spin_unlock_irqrestore(&rcu_lock, flags);

    if (ready)
        rcu_run_callback_list(ready);
}

void RCU_read_lock(void)
{
    if (!rcu_ready)
        return;

    task_preempt_disable();

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        return;

    __atomic_add_fetch(&rcu_cpu[cpu_index].read_depth, 1, __ATOMIC_ACQ_REL);
}

void RCU_read_unlock(void)
{
    if (!rcu_ready)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index < TASK_MAX_CPUS)
    {
        uint32_t depth = __atomic_load_n(&rcu_cpu[cpu_index].read_depth, __ATOMIC_RELAXED);
        if (depth != 0)
            __atomic_sub_fetch(&rcu_cpu[cpu_index].read_depth, 1, __ATOMIC_ACQ_REL);
    }

    task_preempt_enable();
}

bool RCU_call(rcu_callback_fn_t fn, void* context)
{
    if (!rcu_ready || !fn)
        return false;

    rcu_callback_node_t* node = (rcu_callback_node_t*) kmalloc(sizeof(*node));
    if (!node)
        return false;

    node->next = NULL;
    node->fn = fn;
    node->context = context;
    node->target_gp = 0;

    uint64_t flags = spin_lock_irqsave(&rcu_lock);
    uint64_t target = rcu_start_gp_locked();
    node->target_gp = target;

    if (rcu_cb_tail)
        rcu_cb_tail->next = node;
    else
        rcu_cb_head = node;

    rcu_cb_tail = node;
    rcu_cb_pending++;
    spin_unlock_irqrestore(&rcu_lock, flags);

    RCU_note_quiescent_state();
    return true;
}

bool RCU_synchronize(void)
{
    if (!rcu_ready)
        return false;

    uint64_t target = 0;
    uint64_t flags = spin_lock_irqsave(&rcu_lock);
    target = rcu_start_gp_locked();
    spin_unlock_irqrestore(&rcu_lock, flags);

    if (target == 0)
        return true;

    uint64_t start_ticks = ISR_get_timer_ticks();
    while (true)
    {
        uint64_t observed = __atomic_load_n(&rcu_gp_seq, __ATOMIC_ACQUIRE);
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
    if (cpu_index >= TASK_MAX_CPUS)
        cpu_index = 0;

    uint64_t flags = spin_lock_irqsave(&rcu_lock);
    out_stats->gp_seq = rcu_gp_seq;
    out_stats->gp_target = rcu_gp_target;
    out_stats->callbacks_pending = rcu_cb_pending;
    out_stats->local_read_depth = __atomic_load_n(&rcu_cpu[cpu_index].read_depth, __ATOMIC_RELAXED);
    out_stats->local_preempt_count = task_get_preempt_count_cpu(cpu_index);
    spin_unlock_irqrestore(&rcu_lock, flags);
}
