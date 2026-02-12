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

typedef struct FPU_fx_state
{
    uint8_t bytes[512];
} __attribute__((aligned(16))) FPU_fx_state_t;

bool FPU_init_cpu(uint32_t cpu_index);
void FPU_lazy_on_task_switch(void);
void FPU_lazy_probe_current_cpu(void);
bool FPU_is_sse_enabled(void);
bool FPU_is_avx_enabled(void);
bool FPU_stress_ymm_local(uint32_t iterations, uint64_t* signature_out);

#endif
