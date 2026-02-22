#include <CPU/SMP.h>

#include <CPU/APIC.h>
#include <CPU/FPU.h>
#include <CPU/ISR.h>
#include <CPU/x86.h>
#include <Debug/KDebug.h>
#include <Debug/Spinlock.h>
#include <Memory/VMM.h>

#include <string.h>

#include <Task/Task.h>

extern uint8_t SMP_ap_trampoline_start[];
extern uint8_t SMP_ap_trampoline_end[];

static SMP_cpu_local_t SMP_cpu[SMP_MAX_CPUS];
static int32_t SMP_apic_to_cpu_id[SMP_APIC_ID_MAP_SIZE];
static uint32_t SMP_cpu_count = 0;
static uint64_t SMP_pong_irq_total = 0;
static uint32_t SMP_bsp_cpu = 0;
static SMP_tests_t SMP_tests;
static SMP_sched_job_t SMP_sched_jobs[SMP_SCHED_MAX_JOBS];
static uint32_t SMP_sched_patho_short_done = 0;
static uint32_t SMP_sched_patho_long_done = 0;
static uint32_t SMP_sched_patho_short_exec_per_cpu[SMP_MAX_CPUS];
static uint8_t SMP_ymm_done_cpu[SMP_MAX_CPUS];
static uint8_t SMP_ymm_fail_cpu[SMP_MAX_CPUS];
static uint64_t SMP_ymm_signature_cpu[SMP_MAX_CPUS];
static bool SMP_initialized = false;
static uint32_t SMP_bsp_apic_id = 0;

__attribute__((aligned(16))) static uint8_t SMP_ap_stacks[SMP_MAX_CPUS][KERNEL_STACK_SIZE];

static void SMP_ipi_ping_handler(interrupt_frame_t* frame);
static void SMP_ipi_pong_handler(interrupt_frame_t* frame);
static void SMP_ipi_counter_handler(interrupt_frame_t* frame);
static void SMP_ipi_sched_handler(interrupt_frame_t* frame);
static void SMP_ipi_tlb_handler(interrupt_frame_t* frame);
static void SMP_ipi_timer_init_handler(interrupt_frame_t* frame);
static void SMP_setup_ipi_handlers(void);
static bool SMP_validate_ipi_link(void);
static bool SMP_run_counter_stress_test(void);
static bool SMP_run_sched_stress_test(void);
static void SMP_sched_counter_job(void* arg);
static void SMP_sched_balance_job(void* arg);
static bool SMP_run_sched_balance_phase(bool push_enabled, bool steal_enabled, bool bsp_runs_local, const char* label);
static bool SMP_run_sched_balance_tests(void);
static void SMP_sched_patho_long_job(void* arg);
static void SMP_sched_patho_short_job(void* arg);
static bool SMP_run_sched_pathological_test(void);
static void SMP_ymm_stress_job(void* arg);
static bool SMP_run_ymm_stress_test(void);
static bool SMP_validate_tlb_shootdown(void);
static bool SMP_issue_tlb_shootdown(uint8_t kind, uintptr_t virt);

static uintptr_t SMP_read_cr3(void)
{
    uintptr_t cr3 = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static bool SMP_interrupts_enabled(void)
{
    uint64_t rflags = 0;
    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(rflags));
    return (rflags & (1ULL << 9)) != 0;
}

static bool SMP_apic_id_is_mappable(uint32_t apic_id)
{
    return apic_id < SMP_APIC_ID_MAP_SIZE;
}

static uint32_t SMP_cpu_id_from_apic(uint32_t apic_id)
{
    if (!SMP_apic_id_is_mappable(apic_id))
        return SMP_INVALID_CPU_ID;

    int32_t cpu_id = __atomic_load_n(&SMP_apic_to_cpu_id[apic_id], __ATOMIC_ACQUIRE);
    if (cpu_id < 0 || (uint32_t) cpu_id >= SMP_MAX_CPUS)
        return SMP_INVALID_CPU_ID;

    return (uint32_t) cpu_id;
}

static uint32_t SMP_apic_id_from_cpu(uint32_t cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS)
        return SMP_INVALID_APIC_ID;

    return __atomic_load_n(&SMP_cpu[cpu_id].apic_id, __ATOMIC_ACQUIRE);
}

static SMP_cpu_local_t* SMP_cpu_local_from_cpu(uint32_t cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS)
        return NULL;

    return &SMP_cpu[cpu_id];
}

static SMP_cpu_local_t* SMP_cpu_local_from_apic(uint32_t apic_id)
{
    uint32_t cpu_id = SMP_cpu_id_from_apic(apic_id);
    if (cpu_id == SMP_INVALID_CPU_ID)
        return NULL;

    return SMP_cpu_local_from_cpu(cpu_id);
}

static void SMP_mark_cpu_online(uint32_t cpu_index, uint32_t apic_id)
{
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_index);
    if (!cpu)
        return;

    __atomic_store_n(&cpu->cpu_id, cpu_index, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu->apic_id, apic_id, __ATOMIC_RELEASE);

    if (SMP_apic_id_is_mappable(apic_id))
        __atomic_store_n(&SMP_apic_to_cpu_id[apic_id], (int32_t) cpu_index, __ATOMIC_RELEASE);

    uint8_t was_online = __atomic_exchange_n(&cpu->online, 1, __ATOMIC_ACQ_REL);
    if (!was_online)
        __atomic_fetch_add(&SMP_cpu_count, 1, __ATOMIC_ACQ_REL);
}

void SMP_notify_ap_ready(uint32_t cpu_index, uint8_t apic_id)
{
    SMP_mark_cpu_online(cpu_index, (uint32_t) apic_id);
}

bool SMP_send_ipi_to_others(uint8_t vector)
{
    if (!APIC_is_enabled())
        return false;

    bool ok = true;
    uint8_t self_apic = APIC_get_current_lapic_id();
    uint8_t core_count = APIC_get_core_count();

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == self_apic || !SMP_is_apic_online(apic_id))
            continue;

        if (!APIC_send_ipi(apic_id, vector))
            ok = false;
    }

    return ok;
}

static void SMP_local_invlpg(uintptr_t virt)
{
    uintptr_t page = virt & ~(uintptr_t) 0xFFFULL;
    __asm__ __volatile__("invlpg (%0)" : : "r"(page) : "memory");
}

static void SMP_local_reload_cr3(void)
{
    uintptr_t cr3 = 0;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static void SMP_ipi_ping_handler(interrupt_frame_t* frame)
{
    (void) frame;

    uint32_t apic_id = APIC_get_current_lapic_id();
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_apic(apic_id);
    if (cpu)
        __atomic_fetch_add(&cpu->ping_count, 1, __ATOMIC_RELAXED);

    if (SMP_bsp_apic_id <= 0xFFU && APIC_send_ipi((uint8_t) SMP_bsp_apic_id, SMP_IPI_VECTOR_PONG))
    {
        if (cpu)
            __atomic_fetch_add(&cpu->pong_sent_count, 1, __ATOMIC_RELAXED);
    }
    else
    {
        kdebug_printf("[SMP] PONG send failed from apic_id=%u\n", apic_id);
    }

    APIC_send_EOI();
}

static void SMP_ipi_pong_handler(interrupt_frame_t* frame)
{
    (void) frame;
    __atomic_fetch_add(&SMP_pong_irq_total, 1, __ATOMIC_RELAXED);
    APIC_send_EOI();
}

static void SMP_setup_ipi_handlers(void)
{
    ISR_register_vector(SMP_IPI_VECTOR_PING, SMP_ipi_ping_handler);
    ISR_register_vector(SMP_IPI_VECTOR_PONG, SMP_ipi_pong_handler);
    ISR_register_vector(SMP_IPI_VECTOR_COUNTER, SMP_ipi_counter_handler);
    ISR_register_vector(SMP_IPI_VECTOR_SCHED, SMP_ipi_sched_handler);
    ISR_register_vector(SMP_IPI_VECTOR_TLB, SMP_ipi_tlb_handler);
    ISR_register_vector(SMP_IPI_VECTOR_TIMER_INIT, SMP_ipi_timer_init_handler);
}

static void SMP_ipi_counter_handler(interrupt_frame_t* frame)
{
    (void) frame;

    uint32_t cpu_id = task_get_current_cpu_index();
    uint32_t work = 0;
    if (cpu_id < SMP_MAX_CPUS)
        work = SMP_tests.counter_work_per_cpu[cpu_id];

    for (uint32_t i = 0; i < work; i++)
    {
        spin_lock(&SMP_tests.counter_lock);
        __atomic_fetch_add(&SMP_tests.counter_value, 1, __ATOMIC_RELAXED);
        spin_unlock(&SMP_tests.counter_lock);
    }

    if (cpu_id < SMP_MAX_CPUS)
        __atomic_store_n(&SMP_tests.counter_done_cpu[cpu_id], 1, __ATOMIC_RELEASE);

    APIC_send_EOI();
}

static void SMP_ipi_sched_handler(interrupt_frame_t* frame)
{
    (void) frame;

    uint32_t cpu_id = task_get_current_cpu_index();
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    if (cpu)
        __atomic_fetch_add(&cpu->sched_kick_count, 1, __ATOMIC_RELAXED);

    APIC_send_EOI();
}

static void SMP_ipi_tlb_handler(interrupt_frame_t* frame)
{
    (void) frame;

    uint32_t cpu_id = task_get_current_cpu_index();
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    uintptr_t virt = __atomic_load_n(&SMP_tests.tlb_target_virt, __ATOMIC_RELAXED);
    uint8_t kind = __atomic_load_n(&SMP_tests.tlb_kind, __ATOMIC_ACQUIRE);
    uint64_t generation = __atomic_load_n(&SMP_tests.tlb_generation, __ATOMIC_RELAXED);

    if (kind == SMP_TLB_SHOOTDOWN_PAGE)
        SMP_local_invlpg(virt);
    else if (kind == SMP_TLB_SHOOTDOWN_ALL)
        SMP_local_reload_cr3();

    if (cpu)
    {
        __atomic_fetch_add(&cpu->tlb_ipi_count, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->tlb_ack_generation, generation, __ATOMIC_RELEASE);
    }

    APIC_send_EOI();
}

static void SMP_ipi_timer_init_handler(interrupt_frame_t* frame)
{
    (void) frame;

    uint32_t apic_id = APIC_get_current_lapic_id();
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_apic(apic_id);
    if (APIC_timer_init_ap(0))
    {
        if (cpu)
            __atomic_fetch_add(&cpu->timer_start_count, 1, __ATOMIC_RELAXED);
    }
    else
    {
        if (cpu)
            __atomic_fetch_add(&cpu->timer_start_fail_count, 1, __ATOMIC_RELAXED);
        kdebug_printf("[SMP] AP timer init failed on apic_id=%u\n", apic_id);
    }

    APIC_send_EOI();
}

static bool SMP_prepare_trampoline(void)
{
    uintptr_t trampoline_page_phys = SMP_TRAMPOLINE_PHYS & ~(uintptr_t) 0xFFFULL;
    uintptr_t trampoline_page_virt = P2V(trampoline_page_phys);
    uintptr_t trampoline_virt = P2V(SMP_TRAMPOLINE_PHYS);

    // AP startup trampoline executes at low linear addresses right after SIPI.
    VMM_map_page(trampoline_page_phys, trampoline_page_phys);
    VMM_map_page(trampoline_page_virt, trampoline_page_phys);

    size_t trampoline_size = (size_t) (SMP_ap_trampoline_end - SMP_ap_trampoline_start);
    size_t handoff_offset = (size_t) (SMP_HANDOFF_PHYS - SMP_TRAMPOLINE_PHYS);
    if (trampoline_size == 0 || trampoline_size >= handoff_offset)
    {
        kdebug_printf("[SMP] invalid trampoline size=%u\n", (unsigned) trampoline_size);
        return false;
    }

    memset((void*) trampoline_virt, 0, 0x1000);
    memcpy((void*) trampoline_virt, SMP_ap_trampoline_start, trampoline_size);
    return true;
}

static bool SMP_wait_ap_ready(volatile SMP_handoff_t* handoff, uint8_t apic_id)
{
    for (uint32_t spin = 0; spin < SMP_AP_READY_TIMEOUT_LOOPS; spin++)
    {
        if (handoff->ready != 0 && SMP_is_apic_online(apic_id))
            return true;

        __asm__ __volatile__("pause");
    }

    return false;
}

static bool SMP_validate_ipi_link(void)
{
    bool ok = true;
    uint8_t core_count = APIC_get_core_count();
    bool restore_cli = !SMP_interrupts_enabled();

    if (restore_cli)
        sti();

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == SMP_bsp_apic_id || !SMP_is_apic_online(apic_id))
            continue;

        SMP_cpu_local_t* cpu = SMP_cpu_local_from_apic(apic_id);
        if (!cpu)
            continue;

        uint32_t ping_before = __atomic_load_n(&cpu->ping_count, __ATOMIC_RELAXED);
        uint32_t pong_before = __atomic_load_n(&cpu->pong_sent_count, __ATOMIC_RELAXED);
        uint64_t pong_irq_before = __atomic_load_n(&SMP_pong_irq_total, __ATOMIC_RELAXED);

        if (!APIC_send_ipi(apic_id, SMP_IPI_VECTOR_PING))
        {
            kdebug_printf("[SMP] IPI PING send failed apic_id=%u cpu=%u\n", apic_id, cpu_index);
            ok = false;
            continue;
        }

        bool got_reply = false;
        for (uint32_t spin = 0; spin < SMP_IPI_READY_TIMEOUT_LOOPS; spin++)
        {
            if (__atomic_load_n(&cpu->ping_count, __ATOMIC_RELAXED) > ping_before &&
                __atomic_load_n(&cpu->pong_sent_count, __ATOMIC_RELAXED) > pong_before &&
                __atomic_load_n(&SMP_pong_irq_total, __ATOMIC_RELAXED) > pong_irq_before)
            {
                got_reply = true;
                break;
            }

            __asm__ __volatile__("pause");
        }

        if (!got_reply)
        {
            kdebug_printf("[SMP] IPI PING/PONG timeout apic_id=%u cpu=%u ping=%u pong=%u pong_irq=%llu\n",
                          apic_id,
                          cpu_index,
                          __atomic_load_n(&cpu->ping_count, __ATOMIC_RELAXED),
                          __atomic_load_n(&cpu->pong_sent_count, __ATOMIC_RELAXED),
                          (unsigned long long) __atomic_load_n(&SMP_pong_irq_total, __ATOMIC_RELAXED));
            ok = false;
            continue;
        }

        kdebug_printf("[SMP] IPI PING/PONG ok apic_id=%u cpu=%u ping=%u pong=%u\n",
                      apic_id,
                      cpu_index,
                      __atomic_load_n(&cpu->ping_count, __ATOMIC_RELAXED),
                      __atomic_load_n(&cpu->pong_sent_count, __ATOMIC_RELAXED));
    }

    if (restore_cli)
        cli();

    return ok;
}

static bool SMP_run_counter_stress_test(void)
{
    bool ok = true;
    uint8_t core_count = APIC_get_core_count();
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;

    memset((void*) SMP_tests.counter_work_per_cpu, 0, sizeof(SMP_tests.counter_work_per_cpu));
    memset((void*) SMP_tests.counter_done_cpu, 0, sizeof(SMP_tests.counter_done_cpu));
    __atomic_store_n(&SMP_tests.counter_value, 0, __ATOMIC_RELAXED);
    spinlock_init(&SMP_tests.counter_lock);

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == SMP_bsp_apic_id || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    uint32_t participants = target_count + 1U; // APs + BSP
    uint32_t base_work = SMP_COUNTER_STRESS_TARGET / participants;
    uint32_t rem_work = SMP_COUNTER_STRESS_TARGET % participants;

    uint32_t bsp_work = base_work + (rem_work > 0 ? 1U : 0U);
    if (rem_work > 0)
        rem_work--;

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint32_t work = base_work + (rem_work > 0 ? 1U : 0U);
        SMP_tests.counter_work_per_cpu[cpu_id] = work;
        if (rem_work > 0)
            rem_work--;
    }

    bool restore_cli = !SMP_interrupts_enabled();
    if (restore_cli)
        sti();

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t apic_id = apic_targets[i];
        if (!APIC_send_ipi(apic_id, SMP_IPI_VECTOR_COUNTER))
        {
            kdebug_printf("[SMP] counter test: IPI send failed apic_id=%u\n", apic_id);
            ok = false;
        }
    }

    for (uint32_t i = 0; i < bsp_work; i++)
    {
        spin_lock(&SMP_tests.counter_lock);
        __atomic_fetch_add(&SMP_tests.counter_value, 1, __ATOMIC_RELAXED);
        spin_unlock(&SMP_tests.counter_lock);
    }

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        bool done = false;
        for (uint32_t spin = 0; spin < SMP_COUNTER_STRESS_TIMEOUT; spin++)
        {
            if (__atomic_load_n(&SMP_tests.counter_done_cpu[cpu_id], __ATOMIC_ACQUIRE) != 0)
            {
                done = true;
                break;
            }
            __asm__ __volatile__("pause");
        }

        if (!done)
        {
            kdebug_printf("[SMP] counter test timeout cpu=%u apic_id=%u\n", cpu_id, apic_id);
            ok = false;
        }
    }

    if (restore_cli)
        cli();

    uint32_t final_counter = __atomic_load_n(&SMP_tests.counter_value, __ATOMIC_RELAXED);
    if (final_counter != SMP_COUNTER_STRESS_TARGET)
    {
        kdebug_printf("[SMP] counter test FAILED got=%u expected=%u (targets=%u bsp_work=%u)\n",
                      final_counter,
                      SMP_COUNTER_STRESS_TARGET,
                      target_count,
                      bsp_work);
        return false;
    }

    kdebug_printf("[SMP] counter test OK final=%u targets=%u bsp_work=%u\n",
                  final_counter,
                  target_count,
                  bsp_work);
    return ok;
}

static void SMP_sched_counter_job(void* arg)
{
    SMP_sched_job_t* job = (SMP_sched_job_t*) arg;
    if (!job)
        return;

    uint32_t work_count = __atomic_load_n(&job->work, __ATOMIC_RELAXED);
    uint8_t expected_cpu = __atomic_load_n(&job->expected_cpu, __ATOMIC_RELAXED);
    uint32_t current_cpu = task_get_current_cpu_index();
    uint8_t got_cpu = (current_cpu < SMP_MAX_CPUS) ? (uint8_t) current_cpu : 0xFF;
    uint32_t expected_apic = SMP_apic_id_from_cpu(expected_cpu);
    uint32_t got_apic = SMP_apic_id_from_cpu(got_cpu);

    if (got_cpu < SMP_MAX_CPUS)
        __atomic_fetch_add(&SMP_tests.sched_exec_per_cpu[got_cpu], 1, __ATOMIC_RELAXED);

    if (got_cpu != expected_cpu)
    {
        uint32_t err_idx = __atomic_fetch_add(&SMP_tests.sched_migration_errors, 1, __ATOMIC_RELAXED);
        if (err_idx == 0)
        {
            uintptr_t base = (uintptr_t) &SMP_sched_jobs[0];
            uintptr_t self = (uintptr_t) job;
            uint32_t job_index = (self >= base) ? (uint32_t) ((self - base) / sizeof(SMP_sched_jobs[0])) : 0xFFFFFFFFU;
            __atomic_store_n(&SMP_tests.sched_first_migration_job, job_index, __ATOMIC_RELAXED);
            __atomic_store_n(&SMP_tests.sched_first_expected_cpu, expected_cpu, __ATOMIC_RELAXED);
            __atomic_store_n(&SMP_tests.sched_first_got_cpu, got_cpu, __ATOMIC_RELAXED);
            __atomic_store_n(&SMP_tests.sched_first_expected_apic, expected_apic, __ATOMIC_RELAXED);
            __atomic_store_n(&SMP_tests.sched_first_got_apic, got_apic, __ATOMIC_RELAXED);
        }
    }

    for (uint32_t i = 0; i < work_count; i++)
    {
        spin_lock(&SMP_tests.sched_counter_lock);
        __atomic_fetch_add(&SMP_tests.sched_counter_value, 1, __ATOMIC_RELAXED);
        spin_unlock(&SMP_tests.sched_counter_lock);
    }

    __atomic_fetch_add(&SMP_tests.sched_jobs_done, 1, __ATOMIC_RELEASE);
}

static bool SMP_run_sched_stress_test(void)
{
    spinlock_init(&SMP_tests.sched_counter_lock);
    __atomic_store_n(&SMP_tests.sched_counter_value, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_jobs_done, 0, __ATOMIC_RELAXED);
    memset((void*) SMP_tests.sched_exec_per_cpu, 0, sizeof(SMP_tests.sched_exec_per_cpu));
    __atomic_store_n(&SMP_tests.sched_migration_errors, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_migration_job, 0xFFFFFFFFU, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_expected_cpu, 0xFF, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_got_cpu, 0xFF, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_expected_apic, SMP_INVALID_APIC_ID, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_got_apic, SMP_INVALID_APIC_ID, __ATOMIC_RELAXED);
    memset(SMP_sched_jobs, 0, sizeof(SMP_sched_jobs));

    uint8_t core_count = APIC_get_core_count();
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;
    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    if (target_count == 0)
        return true;

    uint32_t job_count = SMP_SCHED_STRESS_JOBS;
    if (job_count == 0 || job_count > SMP_SCHED_STRESS_TARGET)
        job_count = SMP_SCHED_STRESS_TARGET;
    if (job_count > SMP_SCHED_MAX_JOBS)
        job_count = SMP_SCHED_MAX_JOBS;
    if (job_count == 0)
        return true;

    uint32_t base_work = SMP_SCHED_STRESS_TARGET / job_count;
    uint32_t rem_work = SMP_SCHED_STRESS_TARGET % job_count;
    uint32_t expected_jobs_per_cpu[SMP_MAX_CPUS] = { 0 };

    for (uint32_t i = 0; i < job_count; i++)
    {
        uint32_t target_slot = i % target_count;
        uint32_t target_cpu = cpu_targets[target_slot];
        uint8_t target_apic = apic_targets[target_slot];
        uint32_t work_count = base_work + (rem_work > 0 ? 1U : 0U);

        __atomic_store_n(&SMP_sched_jobs[i].work, work_count, __ATOMIC_RELAXED);
        __atomic_store_n(&SMP_sched_jobs[i].expected_cpu, (uint8_t) target_cpu, __ATOMIC_RELEASE);
        if (rem_work > 0)
            rem_work--;

        expected_jobs_per_cpu[target_cpu]++;

        if (!task_schedule_work_on_cpu(target_cpu, SMP_sched_counter_job, &SMP_sched_jobs[i]))
        {
            kdebug_printf("[SMP] scheduler test enqueue failed idx=%u cpu=%u apic=%u depth_cpu=%u depth_total=%u\n",
                          i,
                          target_cpu,
                          target_apic,
                          task_runqueue_depth_cpu(target_cpu),
                          task_runqueue_depth_total());
            return false;
        }
    }

    bool done = false;
    for (uint32_t spin = 0; spin < SMP_SCHED_STRESS_TIMEOUT; spin++)
    {
        while (task_run_next_work())
            ;

        if (__atomic_load_n(&SMP_tests.sched_jobs_done, __ATOMIC_ACQUIRE) == job_count)
        {
            done = true;
            break;
        }

        __asm__ __volatile__("pause");
    }

    if (!done)
    {
        kdebug_printf("[SMP] scheduler test timeout done=%u/%u counter=%u depth_local=%u depth_total=%u\n",
                      __atomic_load_n(&SMP_tests.sched_jobs_done, __ATOMIC_RELAXED),
                      job_count,
                      __atomic_load_n(&SMP_tests.sched_counter_value, __ATOMIC_RELAXED),
                      task_runqueue_depth(),
                      task_runqueue_depth_total());
        return false;
    }

    uint32_t final_counter = __atomic_load_n(&SMP_tests.sched_counter_value, __ATOMIC_RELAXED);
    if (final_counter != SMP_SCHED_STRESS_TARGET)
    {
        kdebug_printf("[SMP] scheduler test FAILED got=%u expected=%u\n",
                      final_counter,
                      SMP_SCHED_STRESS_TARGET);
        return false;
    }

    uint32_t migration_errors = __atomic_load_n(&SMP_tests.sched_migration_errors, __ATOMIC_RELAXED);
    if (migration_errors != 0)
    {
        kdebug_printf("[SMP] scheduler affinity migration job=%u expected cpu=%u apic=%u got cpu=%u apic=%u\n",
                      __atomic_load_n(&SMP_tests.sched_first_migration_job, __ATOMIC_RELAXED),
                      __atomic_load_n(&SMP_tests.sched_first_expected_cpu, __ATOMIC_RELAXED),
                      __atomic_load_n(&SMP_tests.sched_first_expected_apic, __ATOMIC_RELAXED),
                      __atomic_load_n(&SMP_tests.sched_first_got_cpu, __ATOMIC_RELAXED),
                      __atomic_load_n(&SMP_tests.sched_first_got_apic, __ATOMIC_RELAXED));
        kdebug_printf("[SMP] scheduler test FAILED migration_errors=%u\n", migration_errors);
        return false;
    }

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_index = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        uint32_t expected_jobs = expected_jobs_per_cpu[cpu_index];
        uint32_t got_jobs = __atomic_load_n(&SMP_tests.sched_exec_per_cpu[cpu_index], __ATOMIC_RELAXED);
        if (got_jobs != expected_jobs)
        {
            kdebug_printf("[SMP] scheduler test FAILED cpu=%u apic_id=%u expected_jobs=%u got_jobs=%u\n",
                          cpu_index,
                          apic_id,
                          expected_jobs,
                          got_jobs);
            return false;
        }
    }

    kdebug_printf("[SMP] scheduler affinity OK final=%u jobs=%u cpus=%u\n",
                  final_counter,
                  job_count,
                  target_count);
    return true;
}

static void SMP_sched_balance_job(void* arg)
{
    SMP_sched_job_t* job = (SMP_sched_job_t*) arg;
    if (!job)
        return;

    uint32_t work_count = __atomic_load_n(&job->work, __ATOMIC_RELAXED);
    uint32_t cpu_id = task_get_current_cpu_index();
    if (cpu_id < SMP_MAX_CPUS)
        __atomic_fetch_add(&SMP_tests.sched_exec_per_cpu[cpu_id], 1, __ATOMIC_RELAXED);

    for (uint32_t i = 0; i < work_count; i++)
    {
        spin_lock(&SMP_tests.sched_counter_lock);
        __atomic_fetch_add(&SMP_tests.sched_counter_value, 1, __ATOMIC_RELAXED);
        spin_unlock(&SMP_tests.sched_counter_lock);
    }

    __atomic_fetch_add(&SMP_tests.sched_jobs_done, 1, __ATOMIC_RELEASE);
}

static bool SMP_run_sched_balance_phase(bool push_enabled, bool steal_enabled, bool bsp_runs_local, const char* label)
{
    spinlock_init(&SMP_tests.sched_counter_lock);
    __atomic_store_n(&SMP_tests.sched_counter_value, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_jobs_done, 0, __ATOMIC_RELAXED);
    memset((void*) SMP_tests.sched_exec_per_cpu, 0, sizeof(SMP_tests.sched_exec_per_cpu));
    memset(SMP_sched_jobs, 0, sizeof(SMP_sched_jobs));

    task_set_push_balance(push_enabled);
    task_set_work_stealing(steal_enabled);

    uint32_t job_count = SMP_SCHED_BALANCE_JOBS;
    if (job_count == 0 || job_count > SMP_SCHED_BALANCE_TARGET)
        job_count = SMP_SCHED_BALANCE_TARGET;
    if (job_count > SMP_SCHED_MAX_JOBS)
        job_count = SMP_SCHED_MAX_JOBS;
    if (job_count == 0)
        return true;

    uint32_t base_work = SMP_SCHED_BALANCE_TARGET / job_count;
    uint32_t rem_work = SMP_SCHED_BALANCE_TARGET % job_count;
    for (uint32_t i = 0; i < job_count; i++)
    {
        uint32_t work_count = base_work + (rem_work > 0 ? 1U : 0U);
        __atomic_store_n(&SMP_sched_jobs[i].work, work_count, __ATOMIC_RELAXED);
        if (rem_work > 0)
            rem_work--;

        if (!task_schedule_work(SMP_sched_balance_job, &SMP_sched_jobs[i]))
        {
            kdebug_printf("[SMP] scheduler %s enqueue failed idx=%u depth_local=%u depth_total=%u\n",
                          label,
                          i,
                          task_runqueue_depth(),
                          task_runqueue_depth_total());
            return false;
        }
    }

    bool done = false;
    for (uint32_t spin = 0; spin < SMP_SCHED_BALANCE_TIMEOUT; spin++)
    {
        if (bsp_runs_local)
        {
            while (task_run_next_work())
                ;
        }

        if (__atomic_load_n(&SMP_tests.sched_jobs_done, __ATOMIC_ACQUIRE) == job_count)
        {
            done = true;
            break;
        }

        __asm__ __volatile__("pause");
    }

    if (!done)
    {
        kdebug_printf("[SMP] scheduler %s timeout done=%u/%u counter=%u depth_local=%u depth_total=%u\n",
                      label,
                      __atomic_load_n(&SMP_tests.sched_jobs_done, __ATOMIC_RELAXED),
                      job_count,
                      __atomic_load_n(&SMP_tests.sched_counter_value, __ATOMIC_RELAXED),
                      task_runqueue_depth(),
                      task_runqueue_depth_total());
        return false;
    }

    uint32_t final_counter = __atomic_load_n(&SMP_tests.sched_counter_value, __ATOMIC_RELAXED);
    if (final_counter != SMP_SCHED_BALANCE_TARGET)
    {
        kdebug_printf("[SMP] scheduler %s FAILED got=%u expected=%u\n",
                      label,
                      final_counter,
                      SMP_SCHED_BALANCE_TARGET);
        return false;
    }

    uint8_t core_count = APIC_get_core_count();
    uint32_t remote_exec = 0;
    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || !SMP_is_apic_online(apic_id) || cpu_index == SMP_bsp_cpu)
            continue;

        remote_exec += __atomic_load_n(&SMP_tests.sched_exec_per_cpu[cpu_index], __ATOMIC_RELAXED);
    }

    if (SMP_get_online_cpu_count() > 1 && remote_exec == 0)
    {
        kdebug_printf("[SMP] scheduler %s FAILED no remote execution depth_total=%u\n",
                      label,
                      task_runqueue_depth_total());
        return false;
    }

    kdebug_printf("[SMP] scheduler %s OK final=%u jobs=%u remote_exec=%u\n",
                  label,
                  final_counter,
                  job_count,
                  remote_exec);
    return true;
}

static bool SMP_run_sched_balance_tests(void)
{
    if (SMP_get_online_cpu_count() <= 1)
        return true;

    bool saved_push = task_is_push_balance_enabled();
    bool saved_steal = task_is_work_stealing_enabled();

    bool ok_push = SMP_run_sched_balance_phase(true, false, true, "push-balance");
    bool ok_steal = SMP_run_sched_balance_phase(false, true, false, "work-steal");

    task_set_push_balance(saved_push);
    task_set_work_stealing(saved_steal);

    while (task_run_next_work())
        ;

    return ok_push && ok_steal;
}

static void SMP_sched_patho_long_job(void* arg)
{
    (void) arg;

    for (uint32_t spin = 0; spin < SMP_SCHED_PATHO_LONG_SPINS; spin++)
        __asm__ __volatile__("pause");

    __atomic_store_n(&SMP_sched_patho_long_done, 1, __ATOMIC_RELEASE);
}

static void SMP_sched_patho_short_job(void* arg)
{
    (void) arg;

    uint32_t cpu_id = task_get_current_cpu_index();
    if (cpu_id < SMP_MAX_CPUS)
        __atomic_fetch_add(&SMP_sched_patho_short_exec_per_cpu[cpu_id], 1, __ATOMIC_RELAXED);

    __atomic_fetch_add(&SMP_sched_patho_short_done, 1, __ATOMIC_RELEASE);
}

static bool SMP_run_sched_pathological_test(void)
{
    if (SMP_get_online_cpu_count() <= 2)
        return true;

    uint8_t core_count = APIC_get_core_count();
    uint8_t blocking_cpu = 0xFF;
    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || !SMP_is_apic_online(apic_id) || cpu_index == SMP_bsp_cpu)
            continue;

        blocking_cpu = cpu_index;
        break;
    }

    if (blocking_cpu == 0xFF)
        return true;

    bool saved_push = task_is_push_balance_enabled();
    bool saved_steal = task_is_work_stealing_enabled();

    task_set_push_balance(false);
    task_set_work_stealing(true);

    __atomic_store_n(&SMP_sched_patho_short_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_sched_patho_long_done, 0, __ATOMIC_RELAXED);
    memset(SMP_sched_patho_short_exec_per_cpu, 0, sizeof(SMP_sched_patho_short_exec_per_cpu));

    if (!task_schedule_work_on_cpu(blocking_cpu, SMP_sched_patho_long_job, NULL))
    {
        task_set_push_balance(saved_push);
        task_set_work_stealing(saved_steal);
        kdebug_printf("[SMP] scheduler patho enqueue failed (long) cpu=%u\n", blocking_cpu);
        return false;
    }

    for (uint32_t i = 0; i < SMP_SCHED_PATHO_SHORT_JOBS; i++)
    {
        if (!task_schedule_work(SMP_sched_patho_short_job, NULL))
        {
            task_set_push_balance(saved_push);
            task_set_work_stealing(saved_steal);
            kdebug_printf("[SMP] scheduler patho enqueue failed (short) idx=%u depth_total=%u\n",
                          i,
                          task_runqueue_depth_total());
            return false;
        }
    }

    bool done = false;
    for (uint32_t spin = 0; spin < SMP_SCHED_PATHO_TIMEOUT; spin++)
    {
        if (__atomic_load_n(&SMP_sched_patho_short_done, __ATOMIC_ACQUIRE) == SMP_SCHED_PATHO_SHORT_JOBS &&
            __atomic_load_n(&SMP_sched_patho_long_done, __ATOMIC_ACQUIRE) != 0)
        {
            done = true;
            break;
        }

        __asm__ __volatile__("pause");
    }

    task_set_push_balance(saved_push);
    task_set_work_stealing(saved_steal);

    if (!done)
    {
        kdebug_printf("[SMP] scheduler patho timeout short=%u/%u long_done=%u depth_total=%u\n",
                      __atomic_load_n(&SMP_sched_patho_short_done, __ATOMIC_RELAXED),
                      SMP_SCHED_PATHO_SHORT_JOBS,
                      __atomic_load_n(&SMP_sched_patho_long_done, __ATOMIC_RELAXED),
                      task_runqueue_depth_total());
        return false;
    }

    uint32_t non_bsp_short = 0;
    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || !SMP_is_apic_online(apic_id) || cpu_index == SMP_bsp_cpu)
            continue;

        non_bsp_short += __atomic_load_n(&SMP_sched_patho_short_exec_per_cpu[cpu_index], __ATOMIC_RELAXED);
    }

    if (non_bsp_short == 0)
    {
        kdebug_printf("[SMP] scheduler patho FAILED no remote short execution\n");
        return false;
    }

    kdebug_printf("[SMP] scheduler patho OK blocked_cpu=%u short_jobs=%u remote_short=%u blocked_short=%u\n",
                  blocking_cpu,
                  SMP_SCHED_PATHO_SHORT_JOBS,
                  non_bsp_short,
                  __atomic_load_n(&SMP_sched_patho_short_exec_per_cpu[blocking_cpu], __ATOMIC_RELAXED));
    return true;
}

static void SMP_ymm_stress_job(void* arg)
{
    (void) arg;

    uint32_t cpu_id = task_get_current_cpu_index();
    uint64_t signature = 0;
    bool ok = FPU_stress_ymm_local(SMP_YMM_STRESS_ITERS, &signature);

    if (cpu_id < SMP_MAX_CPUS)
    {
        __atomic_store_n(&SMP_ymm_signature_cpu[cpu_id], signature, __ATOMIC_RELAXED);
        __atomic_store_n(&SMP_ymm_fail_cpu[cpu_id], ok ? 0 : 1, __ATOMIC_RELEASE);
        __atomic_store_n(&SMP_ymm_done_cpu[cpu_id], 1, __ATOMIC_RELEASE);
    }
}

static bool SMP_run_ymm_stress_test(void)
{
    if (SMP_get_online_cpu_count() == 0)
        return true;

    if (!FPU_is_avx_enabled())
    {
        kdebug_puts("[SMP] YMM stress skipped (AVX disabled)\n");
        return true;
    }

    memset(SMP_ymm_done_cpu, 0, sizeof(SMP_ymm_done_cpu));
    memset(SMP_ymm_fail_cpu, 0, sizeof(SMP_ymm_fail_cpu));
    memset(SMP_ymm_signature_cpu, 0, sizeof(SMP_ymm_signature_cpu));

    uint8_t core_count = APIC_get_core_count();
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    if (target_count == 0)
        return true;

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        if (!task_schedule_work_on_cpu(cpu_id, SMP_ymm_stress_job, NULL))
        {
            kdebug_printf("[SMP] YMM stress enqueue failed cpu=%u apic_id=%u depth_cpu=%u depth_total=%u\n",
                          cpu_id,
                          apic_id,
                          task_runqueue_depth_cpu(cpu_id),
                          task_runqueue_depth_total());
            return false;
        }
    }

    bool done = false;
    for (uint32_t spin = 0; spin < SMP_YMM_STRESS_TIMEOUT; spin++)
    {
        while (task_run_next_work())
            ;

        uint32_t done_count = 0;
        for (uint32_t i = 0; i < target_count; i++)
        {
            uint8_t cpu_id = cpu_targets[i];
            if (__atomic_load_n(&SMP_ymm_done_cpu[cpu_id], __ATOMIC_ACQUIRE) != 0)
                done_count++;
        }

        if (done_count == target_count)
        {
            done = true;
            break;
        }

        __asm__ __volatile__("pause");
    }

    if (!done)
    {
        uint32_t done_count = 0;
        for (uint32_t i = 0; i < target_count; i++)
        {
            uint8_t cpu_id = cpu_targets[i];
            if (__atomic_load_n(&SMP_ymm_done_cpu[cpu_id], __ATOMIC_RELAXED) != 0)
                done_count++;
        }

        kdebug_printf("[SMP] YMM stress timeout done=%u/%u iters=%u depth_total=%u\n",
                      done_count,
                      target_count,
                      SMP_YMM_STRESS_ITERS,
                      task_runqueue_depth_total());
        return false;
    }

    uint32_t failed_cpu = SMP_INVALID_CPU_ID;
    uint64_t signature_mix = 0;
    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint64_t signature = __atomic_load_n(&SMP_ymm_signature_cpu[cpu_id], __ATOMIC_RELAXED);
        signature_mix ^= (signature + (((uint64_t) cpu_id + 1ULL) << 32));

        if (__atomic_load_n(&SMP_ymm_fail_cpu[cpu_id], __ATOMIC_ACQUIRE) != 0 && failed_cpu == SMP_INVALID_CPU_ID)
            failed_cpu = cpu_id;
    }

    if (failed_cpu != SMP_INVALID_CPU_ID)
    {
        kdebug_printf("[SMP] YMM stress FAILED cpu=%u iters=%u\n",
                      failed_cpu,
                      SMP_YMM_STRESS_ITERS);
        return false;
    }

    kdebug_printf("[SMP] YMM stress OK cpus=%u iters=%u sig=0x%llX\n",
                  target_count,
                  SMP_YMM_STRESS_ITERS,
                  (unsigned long long) signature_mix);
    return true;
}

static bool SMP_issue_tlb_shootdown(uint8_t kind, uintptr_t virt)
{
    if (!APIC_is_enabled() || __atomic_load_n(&SMP_cpu_count, __ATOMIC_ACQUIRE) <= 1)
        return true;

    uint8_t self_apic = APIC_get_current_lapic_id();
    uint8_t core_count = APIC_get_core_count();
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == self_apic || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    if (target_count == 0)
        return true;

    bool ok = true;
    uint64_t flags = spin_lock_irqsave(&SMP_tests.tlb_lock);
    uint64_t generation = __atomic_add_fetch(&SMP_tests.tlb_generation, 1, __ATOMIC_ACQ_REL);

    __atomic_store_n(&SMP_tests.tlb_target_virt, virt & ~(uintptr_t) 0xFFFULL, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.tlb_kind, kind, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        if (cpu)
            __atomic_store_n(&cpu->tlb_ack_generation, generation - 1, __ATOMIC_RELAXED);
    }

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        if (!APIC_send_ipi(apic_id, SMP_IPI_VECTOR_TLB))
        {
            kdebug_printf("[SMP] TLB shootdown IPI send failed cpu=%u apic_id=%u\n", cpu_id, apic_id);
            SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
            if (cpu)
                __atomic_store_n(&cpu->tlb_ack_generation, generation, __ATOMIC_RELAXED);
            ok = false;
        }
    }

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        bool acked = false;
        for (uint32_t spin = 0; spin < SMP_TLB_SHOOTDOWN_TIMEOUT; spin++)
        {
            if (cpu && __atomic_load_n(&cpu->tlb_ack_generation, __ATOMIC_ACQUIRE) >= generation)
            {
                acked = true;
                break;
            }
            __asm__ __volatile__("pause");
        }

        if (!acked)
        {
            kdebug_printf("[SMP] TLB shootdown timeout cpu=%u apic_id=%u gen=%llu\n",
                          cpu_id,
                          apic_id,
                          (unsigned long long) generation);
            ok = false;
        }
    }

    __atomic_store_n(&SMP_tests.tlb_kind, SMP_TLB_SHOOTDOWN_NONE, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&SMP_tests.tlb_lock, flags);

    return ok;
}

bool SMP_tlb_shootdown_page(uintptr_t virt)
{
    SMP_local_invlpg(virt);
    return SMP_issue_tlb_shootdown(SMP_TLB_SHOOTDOWN_PAGE, virt);
}

bool SMP_tlb_shootdown_all(void)
{
    SMP_local_reload_cr3();
    return SMP_issue_tlb_shootdown(SMP_TLB_SHOOTDOWN_ALL, 0);
}

static bool SMP_validate_tlb_shootdown(void)
{
    if (__atomic_load_n(&SMP_cpu_count, __ATOMIC_ACQUIRE) <= 1)
        return true;

    uint8_t core_count = APIC_get_core_count();
    uint8_t self_apic = APIC_get_current_lapic_id();
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == self_apic || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    if (target_count == 0)
        return true;

    uint64_t before[SMP_MAX_CPUS] = { 0 };
    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        if (cpu)
            before[cpu_id] = __atomic_load_n(&cpu->tlb_ipi_count, __ATOMIC_RELAXED);
    }

    if (!SMP_tlb_shootdown_page((uintptr_t) &SMP_initialized))
        return false;

    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        uint64_t after = cpu ? __atomic_load_n(&cpu->tlb_ipi_count, __ATOMIC_RELAXED) : 0;
        if (after == before[cpu_id])
        {
            kdebug_printf("[SMP] TLB shootdown test missed cpu=%u apic_id=%u\n", cpu_id, apic_id);
            return false;
        }
    }

    kdebug_printf("[SMP] TLB shootdown test OK targets=%u\n", target_count);
    return true;
}

bool SMP_init(void)
{
    if (SMP_initialized)
        return true;

    for (uint32_t cpu_id = 0; cpu_id < SMP_MAX_CPUS; cpu_id++)
    {
        SMP_cpu_local_t* cpu = &SMP_cpu[cpu_id];
        cpu->cpu_id = cpu_id;
        cpu->apic_id = SMP_INVALID_APIC_ID;
        cpu->kstack_top = 0;
        __atomic_store_n(&cpu->online, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->ping_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->pong_sent_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->sched_kick_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->tlb_ipi_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->tlb_ack_generation, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->timer_start_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cpu->timer_start_fail_count, 0, __ATOMIC_RELAXED);
    }

    for (uint32_t apic_id = 0; apic_id < SMP_APIC_ID_MAP_SIZE; apic_id++)
        __atomic_store_n(&SMP_apic_to_cpu_id[apic_id], -1, __ATOMIC_RELAXED);

    memset(&SMP_tests, 0, sizeof(SMP_tests));
    memset(SMP_sched_patho_short_exec_per_cpu, 0, sizeof(SMP_sched_patho_short_exec_per_cpu));
    memset(SMP_ymm_done_cpu, 0, sizeof(SMP_ymm_done_cpu));
    memset(SMP_ymm_fail_cpu, 0, sizeof(SMP_ymm_fail_cpu));
    memset(SMP_ymm_signature_cpu, 0, sizeof(SMP_ymm_signature_cpu));
    __atomic_store_n(&SMP_sched_patho_short_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_sched_patho_long_done, 0, __ATOMIC_RELAXED);
    spinlock_init(&SMP_tests.counter_lock);
    spinlock_init(&SMP_tests.sched_counter_lock);
    spinlock_init(&SMP_tests.tlb_lock);
    __atomic_store_n(&SMP_tests.sched_first_migration_job, 0xFFFFFFFFU, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_expected_cpu, 0xFF, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_got_cpu, 0xFF, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_expected_apic, SMP_INVALID_APIC_ID, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.sched_first_got_apic, SMP_INVALID_APIC_ID, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_tests.tlb_kind, SMP_TLB_SHOOTDOWN_NONE, __ATOMIC_RELAXED);

    __atomic_store_n(&SMP_pong_irq_total, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&SMP_cpu_count, 0, __ATOMIC_RELAXED);

    SMP_bsp_apic_id = APIC_get_bsp_lapic_id();
    SMP_bsp_cpu = 0;
    SMP_mark_cpu_online(SMP_bsp_cpu, SMP_bsp_apic_id);
    kdebug_printf("[SMP] init start bsp_apic_id=%u\n", (unsigned) SMP_bsp_apic_id);

    SMP_setup_ipi_handlers();

    if (!APIC_is_enabled())
    {
        SMP_initialized = true;
        return true;
    }

    uint8_t core_count = APIC_get_core_count();
    kdebug_printf("[SMP] detected cores=%u\n", (unsigned) core_count);
    if (core_count <= 1)
    {
        SMP_initialized = true;
        return true;
    }

    if (!SMP_prepare_trampoline())
        return false;

    volatile SMP_handoff_t* handoff = (volatile SMP_handoff_t*) P2V(SMP_HANDOFF_PHYS);
    uintptr_t cr3 = SMP_read_cr3();

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == SMP_bsp_apic_id)
            continue;
        kdebug_printf("[SMP] startup begin apic_id=%u cpu=%u\n", apic_id, cpu_index);

        uintptr_t stack_top = (uintptr_t) &SMP_ap_stacks[cpu_index][KERNEL_STACK_SIZE];
        stack_top &= ~(uintptr_t) 0xFULL;
        SMP_cpu[cpu_index].kstack_top = stack_top;

        memset((void*) handoff, 0, sizeof(*handoff));
        handoff->magic = SMP_HANDOFF_MAGIC_VALUE;
        handoff->cr3 = (uint64_t) cr3;
        handoff->stack_top = (uint64_t) stack_top;
        handoff->entry64 = (uint64_t) (uintptr_t) SMP_ap_entry;
        handoff->arg = (uint64_t) (uintptr_t) SMP_HANDOFF_PHYS;
        handoff->apic_id = apic_id;
        handoff->cpu_index = cpu_index;
        handoff->ready = 0;
        __asm__ __volatile__("" : : : "memory");

        if (!APIC_startup_ap(apic_id, (uint8_t) SMP_TRAMPOLINE_VECTOR))
        {
            kdebug_printf("[SMP] AP startup IPI failed apic_id=%u cpu=%u\n", apic_id, cpu_index);
            continue;
        }

        if (!SMP_wait_ap_ready(handoff, apic_id))
        {
            kdebug_printf("[SMP] AP ready timeout apic_id=%u cpu=%u\n", apic_id, cpu_index);
            continue;
        }
    }

    if (!SMP_validate_ipi_link())
        kdebug_puts("[SMP] IPI validation failed on one or more APs\n");

#if SMP_COUNTER_STRESS_ENABLE
    if (!SMP_run_counter_stress_test())
        kdebug_puts("[SMP] counter stress test reported failures\n");
#endif

#if SMP_SCHED_STRESS_ENABLE
    if (!SMP_run_sched_stress_test())
        kdebug_puts("[SMP] scheduler stress test reported failures\n");
#endif

#if SMP_SCHED_BALANCE_TEST_ENABLE
    if (!SMP_run_sched_balance_tests())
        kdebug_puts("[SMP] scheduler balance tests reported failures\n");
#endif

#if SMP_SCHED_PATHO_TEST_ENABLE
    if (!SMP_run_sched_pathological_test())
        kdebug_puts("[SMP] scheduler pathological test reported failures\n");
#endif

#if SMP_YMM_STRESS_ENABLE
    if (!SMP_run_ymm_stress_test())
        kdebug_puts("[SMP] YMM stress test reported failures\n");
#endif

#if SMP_TLB_TEST_ENABLE
    if (!SMP_validate_tlb_shootdown())
        kdebug_puts("[SMP] TLB shootdown validation failed\n");
#endif

    SMP_initialized = true;
    return true;
}

uint32_t SMP_get_current_cpu(void)
{
    uint32_t apic_id = APIC_get_current_lapic_id();
    uint32_t cpu_id = SMP_cpu_id_from_apic(apic_id);
    if (cpu_id == SMP_INVALID_CPU_ID)
        return SMP_bsp_cpu;

    return cpu_id;
}

bool SMP_is_apic_online(uint8_t apic_id)
{
    uint32_t cpu_id = SMP_cpu_id_from_apic(apic_id);
    if (cpu_id == SMP_INVALID_CPU_ID)
        return false;

    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    if (!cpu)
        return false;

    return __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) != 0;
}

uint8_t SMP_get_online_cpu_count(void)
{
    uint32_t count = __atomic_load_n(&SMP_cpu_count, __ATOMIC_ACQUIRE);
    if (count > 0xFFU)
        count = 0xFFU;
    return (uint8_t) count;
}

uint32_t SMP_get_ping_count(uint8_t apic_id)
{
    uint32_t cpu_id = SMP_cpu_id_from_apic(apic_id);
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    if (!cpu)
        return 0;

    return __atomic_load_n(&cpu->ping_count, __ATOMIC_RELAXED);
}

uint32_t SMP_get_pong_count(uint8_t apic_id)
{
    uint32_t cpu_id = SMP_cpu_id_from_apic(apic_id);
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    if (!cpu)
        return 0;

    return __atomic_load_n(&cpu->pong_sent_count, __ATOMIC_RELAXED);
}

uint64_t SMP_get_sched_kick_count(uint32_t cpu_id)
{
    SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
    if (!cpu)
        return 0;

    return __atomic_load_n(&cpu->sched_kick_count, __ATOMIC_RELAXED);
}

bool SMP_start_ap_timers(void)
{
    if (!APIC_is_enabled() || !SMP_initialized)
        return false;

    if (SMP_get_online_cpu_count() <= 1)
        return true;

    uint8_t core_count = APIC_get_core_count();
    uint8_t apic_targets[SMP_MAX_CPUS];
    uint8_t cpu_targets[SMP_MAX_CPUS];
    uint32_t target_count = 0;

    for (uint8_t cpu_index = 0; cpu_index < core_count; cpu_index++)
    {
        uint8_t apic_id = APIC_get_core_id(cpu_index);
        if (apic_id == 0xFF || apic_id == APIC_get_bsp_lapic_id() || !SMP_is_apic_online(apic_id))
            continue;

        if (target_count < SMP_MAX_CPUS)
        {
            cpu_targets[target_count] = cpu_index;
            apic_targets[target_count] = apic_id;
            target_count++;
        }
    }

    if (target_count == 0)
        return true;

    uint32_t ok_before[SMP_MAX_CPUS] = { 0 };
    uint32_t fail_before[SMP_MAX_CPUS] = { 0 };
    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        if (!cpu)
            continue;

        ok_before[cpu_id] = __atomic_load_n(&cpu->timer_start_count, __ATOMIC_RELAXED);
        fail_before[cpu_id] = __atomic_load_n(&cpu->timer_start_fail_count, __ATOMIC_RELAXED);
    }

    bool restore_cli = !SMP_interrupts_enabled();
    if (restore_cli)
        sti();

    bool send_ok = true;
    for (uint32_t i = 0; i < target_count; i++)
    {
        if (!APIC_send_ipi(apic_targets[i], SMP_IPI_VECTOR_TIMER_INIT))
        {
            kdebug_printf("[SMP] AP timer init IPI send failed apic_id=%u cpu=%u\n",
                          apic_targets[i],
                          cpu_targets[i]);
            send_ok = false;
        }
    }

    bool all_acked = true;
    for (uint32_t i = 0; i < target_count; i++)
    {
        uint8_t cpu_id = cpu_targets[i];
        uint8_t apic_id = apic_targets[i];
        SMP_cpu_local_t* cpu = SMP_cpu_local_from_cpu(cpu_id);
        bool acked = false;
        bool started = false;

        for (uint32_t spin = 0; spin < SMP_TIMER_INIT_TIMEOUT; spin++)
        {
            if (cpu)
            {
                uint32_t ok_now = __atomic_load_n(&cpu->timer_start_count, __ATOMIC_ACQUIRE);
                uint32_t fail_now = __atomic_load_n(&cpu->timer_start_fail_count, __ATOMIC_ACQUIRE);
                if (ok_now > ok_before[cpu_id])
                {
                    acked = true;
                    started = true;
                    break;
                }
                if (fail_now > fail_before[cpu_id])
                {
                    acked = true;
                    started = false;
                    break;
                }
            }
            __asm__ __volatile__("pause");
        }

        if (!acked)
        {
            kdebug_printf("[SMP] AP timer init timeout apic_id=%u cpu=%u\n", apic_id, cpu_id);
            all_acked = false;
            continue;
        }

        if (started)
            kdebug_printf("[SMP] AP timer active apic_id=%u cpu=%u\n", apic_id, cpu_id);
        else
        {
            kdebug_printf("[SMP] AP timer init ack failed apic_id=%u cpu=%u\n", apic_id, cpu_id);
            all_acked = false;
        }
    }

    if (restore_cli)
        cli();

    return send_ok && all_acked;
}
