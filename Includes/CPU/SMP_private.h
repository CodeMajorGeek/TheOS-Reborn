#ifndef _SMP_PRIVATE_H
#define _SMP_PRIVATE_H

#include <CPU/SMP.h>
#include <CPU/ISR.h>

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

#endif
