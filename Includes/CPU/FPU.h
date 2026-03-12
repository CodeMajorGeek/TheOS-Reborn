#ifndef _FPU_H
#define _FPU_H

#include <stdbool.h>
#include <stdint.h>

#define FPU_CR0_MP             (1ULL << 1)
#define FPU_CR0_EM             (1ULL << 2)
#define FPU_CR0_NE             (1ULL << 5)

#define FPU_CR4_OSFXSR         (1ULL << 9)
#define FPU_CR4_OSXMMEXCPT     (1ULL << 10)
#define FPU_CR4_OSXSAVE        (1ULL << 18)

#define FPU_XCR0_X87           (1ULL << 0)
#define FPU_XCR0_SSE           (1ULL << 1)
#define FPU_XCR0_AVX           (1ULL << 2)

#define FPU_XSAVE_CPUID_LEAF   0xDU
#define FPU_XSAVE_ALIGN        64U
#define FPU_XSAVE_AREA_MAX     4096U
#define FPU_STRESS_CPU_SLOTS   256U

typedef struct FPU_fx_state
{
    uint8_t bytes[512];
} __attribute__((aligned(16))) FPU_fx_state_t;

typedef struct FPU_xsave_buffer
{
    uint8_t bytes[FPU_XSAVE_AREA_MAX];
} __attribute__((aligned(FPU_XSAVE_ALIGN))) FPU_xsave_buffer_t;

typedef struct FPU_runtime_state
{
    FPU_xsave_buffer_t initial_state;
    bool initial_state_ready;
    bool sse_enabled;
    bool avx_enabled;
    uint8_t reserved0;
    uint32_t state_area_size;
    uint64_t state_mask;
    FPU_xsave_buffer_t stress_state_a[FPU_STRESS_CPU_SLOTS];
    FPU_xsave_buffer_t stress_state_b[FPU_STRESS_CPU_SLOTS];
} FPU_runtime_state_t;

struct task; /* forward declaration for FPU context API */

bool FPU_init_cpu(uint32_t cpu_index);
void FPU_switch_task(struct task* prev, struct task* next);
bool FPU_is_sse_enabled(void);
bool FPU_is_avx_enabled(void);
bool FPU_stress_ymm_local(uint32_t iterations, uint64_t* signature_out);

#endif
