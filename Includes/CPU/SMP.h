#ifndef _SMP_H
#define _SMP_H

#define SMP_MAX_CPUS                256
#define SMP_TRAMPOLINE_PHYS         0x7000
#define SMP_HANDOFF_PHYS            0x7E00
#define SMP_TRAMPOLINE_VECTOR       (SMP_TRAMPOLINE_PHYS >> 12)

#define SMP_HANDOFF_MAGIC_VALUE     0x41505F48414E444F

#define SMP_HANDOFF_MAGIC_OFF       0
#define SMP_HANDOFF_CR3_OFF         8
#define SMP_HANDOFF_STACK_TOP_OFF   16
#define SMP_HANDOFF_ENTRY64_OFF     24
#define SMP_HANDOFF_ARG_OFF         32
#define SMP_HANDOFF_APIC_ID_OFF     40
#define SMP_HANDOFF_CPU_INDEX_OFF   44
#define SMP_HANDOFF_READY_OFF       48
#define SMP_HANDOFF_SIZE            56

#define SMP_IPI_VECTOR_PING         0xF1
#define SMP_IPI_VECTOR_PONG         0xF2
#define SMP_IPI_VECTOR_COUNTER      0xF3
#define SMP_IPI_VECTOR_SCHED        0xF4
#define SMP_IPI_VECTOR_TLB          0xF5
#define SMP_IPI_VECTOR_TIMER_INIT   0xF6

#ifndef ASM_FILE

#include <stdint.h>
#include <stdbool.h>

#include <Debug/Spinlock.h>

#define SMP_APIC_ID_MAP_SIZE         1024U
#define SMP_INVALID_APIC_ID          0xFFFFFFFFU
#define SMP_INVALID_CPU_ID           0xFFFFFFFFU

#define SMP_AP_READY_TIMEOUT_LOOPS   200000000U
#define SMP_IPI_READY_TIMEOUT_LOOPS  50000000U
#define SMP_COUNTER_STRESS_TARGET    1000000U
#define SMP_COUNTER_STRESS_TIMEOUT   200000000U
#define SMP_COUNTER_STRESS_ENABLE    1
#define SMP_SCHED_STRESS_TARGET      1000000U
#define SMP_SCHED_STRESS_JOBS        128U
#define SMP_SCHED_STRESS_TIMEOUT     300000000U
#define SMP_SCHED_STRESS_ENABLE      1
#define SMP_SCHED_BALANCE_TARGET     200000U
#define SMP_SCHED_BALANCE_JOBS       128U
#define SMP_SCHED_BALANCE_TIMEOUT    300000000U
#define SMP_SCHED_BALANCE_TEST_ENABLE 1
#define SMP_SCHED_PATHO_SHORT_JOBS   256U
#define SMP_SCHED_PATHO_TIMEOUT      300000000U
#define SMP_SCHED_PATHO_LONG_SPINS   60000000U
#define SMP_SCHED_PATHO_TEST_ENABLE  1
#define SMP_SCHED_MAX_JOBS           ((SMP_SCHED_STRESS_JOBS > SMP_SCHED_BALANCE_JOBS) ? SMP_SCHED_STRESS_JOBS : SMP_SCHED_BALANCE_JOBS)
#define SMP_TLB_SHOOTDOWN_TIMEOUT    50000000U
#define SMP_TLB_TEST_ENABLE          1
#define SMP_YMM_STRESS_ENABLE        1
#define SMP_YMM_STRESS_ITERS         4096U
#define SMP_YMM_STRESS_TIMEOUT       300000000U
#define SMP_TIMER_INIT_TIMEOUT       50000000U

typedef enum SMP_tlb_shootdown_kind
{
    SMP_TLB_SHOOTDOWN_NONE = 0,
    SMP_TLB_SHOOTDOWN_PAGE = 1,
    SMP_TLB_SHOOTDOWN_ALL = 2
} SMP_tlb_shootdown_kind_t;

typedef struct SMP_handoff
{
    uint64_t magic;
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t entry64;
    uint64_t arg;
    uint32_t apic_id;
    uint32_t cpu_index;
    volatile uint32_t ready;
    uint32_t rsv;
} __attribute__((__packed__)) SMP_handoff_t;

typedef struct SMP_sched_job
{
    uint32_t work;
    uint8_t expected_cpu;
    uint8_t rsv[3];
} SMP_sched_job_t;

typedef struct SMP_cpu_local
{
    uint32_t apic_id;
    uint32_t cpu_id;
    uint8_t online;

    uint32_t ping_count;
    uint32_t pong_sent_count;
    uint64_t sched_kick_count;
    uint64_t tlb_ipi_count;
    uint64_t tlb_ack_generation;
    uint32_t timer_start_count;
    uint32_t timer_start_fail_count;

    uintptr_t kstack_top;
} SMP_cpu_local_t;

typedef struct SMP_tests
{
    spinlock_t counter_lock;
    uint32_t counter_value;
    uint32_t counter_work_per_cpu[SMP_MAX_CPUS];
    uint8_t counter_done_cpu[SMP_MAX_CPUS];

    spinlock_t sched_counter_lock;
    uint32_t sched_counter_value;
    uint32_t sched_jobs_done;
    uint32_t sched_exec_per_cpu[SMP_MAX_CPUS];
    uint32_t sched_migration_errors;
    uint32_t sched_first_migration_job;
    uint8_t sched_first_expected_cpu;
    uint8_t sched_first_got_cpu;
    uint32_t sched_first_expected_apic;
    uint32_t sched_first_got_apic;

    spinlock_t tlb_lock;
    uint64_t tlb_generation;
    uintptr_t tlb_target_virt;
    uint8_t tlb_kind;
} SMP_tests_t;

bool SMP_init(void);
uint32_t SMP_get_current_cpu(void);
bool SMP_is_apic_online(uint8_t apic_id);
uint8_t SMP_get_online_cpu_count(void);
void SMP_ap_entry(uintptr_t handoff_phys);
void SMP_notify_ap_ready(uint32_t cpu_index, uint8_t apic_id);
uint32_t SMP_get_ping_count(uint8_t apic_id);
uint32_t SMP_get_pong_count(uint8_t apic_id);
uint64_t SMP_get_sched_kick_count(uint32_t cpu_id);
bool SMP_send_ipi_to_others(uint8_t vector);
bool SMP_tlb_shootdown_page(uintptr_t virt);
bool SMP_tlb_shootdown_all(void);
bool SMP_start_ap_timers(void);

#endif
#endif
