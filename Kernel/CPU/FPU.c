#include <CPU/FPU.h>

#include <CPU/ISR.h>
#include <CPU/x86.h>
#include <Debug/KDebug.h>
#include <Memory/KMem.h>
#include <Task/Task.h>

#include <cpuid.h>
#include <string.h>

static uint8_t FPU_initial_state[FPU_XSAVE_AREA_MAX] __attribute__((aligned(FPU_XSAVE_ALIGN)));
static bool FPU_initial_state_ready = false;
static bool FPU_nm_handler_registered = false;
static bool FPU_sse_enabled = false;
static bool FPU_avx_enabled = false;
static uint32_t FPU_state_area_size = sizeof(FPU_fx_state_t);
static uint64_t FPU_state_mask = FPU_XCR0_X87 | FPU_XCR0_SSE;
static uint8_t FPU_stress_state_a[TASK_MAX_CPUS][FPU_XSAVE_AREA_MAX] __attribute__((aligned(FPU_XSAVE_ALIGN)));
static uint8_t FPU_stress_state_b[TASK_MAX_CPUS][FPU_XSAVE_AREA_MAX] __attribute__((aligned(FPU_XSAVE_ALIGN)));

static task_t* FPU_owner[TASK_MAX_CPUS] = { 0 };
static uint64_t FPU_nm_hits[TASK_MAX_CPUS] = { 0 };

static void FPU_nm_handler(interrupt_frame_t* frame);

static inline void FPU_cpuid_full(uint32_t leaf,
                                  uint32_t subleaf,
                                  uint32_t* eax,
                                  uint32_t* ebx,
                                  uint32_t* ecx,
                                  uint32_t* edx)
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(leaf), "c"(subleaf));
    if (eax)
        *eax = a;
    if (ebx)
        *ebx = b;
    if (ecx)
        *ecx = c;
    if (edx)
        *edx = d;
}

static inline void FPU_fxsave_to(FPU_fx_state_t* state)
{
    __asm__ __volatile__("fxsave %0" : "=m"(*state) : : "memory");
}

static inline void FPU_fxrstor_from(const FPU_fx_state_t* state)
{
    __asm__ __volatile__("fxrstor %0" : : "m"(*state) : "memory");
}

static inline void FPU_xsave_to(void* state)
{
    uint32_t eax = (uint32_t) (FPU_state_mask & 0xFFFFFFFFULL);
    uint32_t edx = (uint32_t) ((FPU_state_mask >> 32) & 0xFFFFFFFFULL);
    __asm__ __volatile__("xsave (%0)" : : "r"(state), "a"(eax), "d"(edx) : "memory");
}

static inline void FPU_xrstor_from(const void* state)
{
    uint32_t eax = (uint32_t) (FPU_state_mask & 0xFFFFFFFFULL);
    uint32_t edx = (uint32_t) ((FPU_state_mask >> 32) & 0xFFFFFFFFULL);
    __asm__ __volatile__("xrstor (%0)" : : "r"(state), "a"(eax), "d"(edx) : "memory");
}

static inline void FPU_save_state(void* state)
{
    if (FPU_avx_enabled)
        FPU_xsave_to(state);
    else
        FPU_fxsave_to((FPU_fx_state_t*) state);
}

static inline void FPU_restore_state(const void* state)
{
    if (FPU_avx_enabled)
        FPU_xrstor_from(state);
    else
        FPU_fxrstor_from((const FPU_fx_state_t*) state);
}

static inline uintptr_t FPU_align_up_uintptr(uintptr_t value, uintptr_t align)
{
    return (value + (align - 1U)) & ~(align - 1U);
}

static bool FPU_ensure_task_state(task_t* task)
{
    if (!task)
        return false;

    if (task->fpu_state_ptr != 0 &&
        task->fpu_state_size >= FPU_state_area_size &&
        (task->fpu_state_ptr & (FPU_XSAVE_ALIGN - 1U)) == 0)
    {
        return true;
    }

    if (task->fpu_state_alloc != 0)
    {
        kfree((void*) task->fpu_state_alloc);
        task->fpu_state_alloc = 0;
        task->fpu_state_ptr = 0;
        task->fpu_state_size = 0;
    }

    size_t alloc_size = (size_t) FPU_state_area_size + (size_t) FPU_XSAVE_ALIGN - 1U;
    void* raw = kmalloc(alloc_size);
    if (!raw)
        return false;

    uintptr_t aligned = FPU_align_up_uintptr((uintptr_t) raw, (uintptr_t) FPU_XSAVE_ALIGN);
    task->fpu_state_alloc = (uintptr_t) raw;
    task->fpu_state_ptr = aligned;
    task->fpu_state_size = FPU_state_area_size;
    memset((void*) aligned, 0, FPU_state_area_size);
    return true;
}

static inline void FPU_save_task(task_t* task)
{
    if (!task || !task->fpu_initialized)
        return;

    if (!FPU_ensure_task_state(task))
    {
        task->fpu_initialized = 0;
        return;
    }

    FPU_save_state((void*) task->fpu_state_ptr);
}

static inline bool FPU_restore_task(task_t* task)
{
    if (!task || !task->fpu_initialized)
        return false;

    if (!FPU_ensure_task_state(task))
    {
        task->fpu_initialized = 0;
        return false;
    }

    FPU_restore_state((const void*) task->fpu_state_ptr);
    return true;
}

bool FPU_init_cpu(uint32_t cpu_index)
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    FPU_cpuid_full(1, 0, &eax, &ebx, &ecx, &edx);

    bool has_fxsr = (edx & CPUID_FEAT_EDX_FXSR) != 0;
    bool has_sse = (edx & CPUID_FEAT_EDX_SSE) != 0;
    if (!has_fxsr || !has_sse)
        return false;

    uint32_t max_basic_leaf = 0;
    FPU_cpuid_full(0, 0, &max_basic_leaf, NULL, NULL, NULL);

    bool has_xsave = (ecx & CPUID_FEAT_ECX_XSAVE) != 0;
    bool has_avx = (ecx & CPUID_FEAT_ECX_AVX) != 0;
    bool enable_avx = has_xsave && has_avx && (max_basic_leaf >= FPU_XSAVE_CPUID_LEAF);

    uint64_t cr0 = x86_read_cr0();
    cr0 |= FPU_CR0_MP | FPU_CR0_NE;
    cr0 &= ~FPU_CR0_EM;
    x86_write_cr0(cr0);

    uint64_t cr4 = x86_read_cr4();
    cr4 |= FPU_CR4_OSFXSR | FPU_CR4_OSXMMEXCPT;
    if (enable_avx)
        cr4 |= FPU_CR4_OSXSAVE;
    x86_write_cr4(cr4);

    if (enable_avx)
    {
        uint64_t xcr0 = x86_xgetbv(0);
        xcr0 |= (FPU_XCR0_X87 | FPU_XCR0_SSE | FPU_XCR0_AVX);
        x86_xsetbv(0, xcr0);

        uint32_t xsave_size = 0;
        FPU_cpuid_full(FPU_XSAVE_CPUID_LEAF, 0, &xsave_size, NULL, NULL, NULL);
        if (xsave_size >= sizeof(FPU_fx_state_t) && xsave_size <= FPU_XSAVE_AREA_MAX)
        {
            FPU_state_area_size = xsave_size;
            FPU_state_mask = x86_xgetbv(0);
            FPU_avx_enabled = true;
        }
        else
        {
            uint64_t fallback_mask = x86_xgetbv(0);
            fallback_mask &= ~FPU_XCR0_AVX;
            fallback_mask |= (FPU_XCR0_X87 | FPU_XCR0_SSE);
            x86_xsetbv(0, fallback_mask);

            FPU_state_area_size = sizeof(FPU_fx_state_t);
            FPU_state_mask = FPU_XCR0_X87 | FPU_XCR0_SSE;
            FPU_avx_enabled = false;
        }
    }

    if (!FPU_avx_enabled)
    {
        FPU_state_area_size = sizeof(FPU_fx_state_t);
        FPU_state_mask = FPU_XCR0_X87 | FPU_XCR0_SSE;
    }

    x86_clear_ts();

    if (!FPU_initial_state_ready)
    {
        __asm__ __volatile__("fninit");
        memset(FPU_initial_state, 0, sizeof(FPU_initial_state));
        FPU_save_state(FPU_initial_state);
        FPU_initial_state_ready = true;
    }

    if (!FPU_nm_handler_registered)
    {
        ISR_register_vector(7, FPU_nm_handler);
        FPU_nm_handler_registered = true;
    }

    if (cpu_index < TASK_MAX_CPUS)
    {
        FPU_owner[cpu_index] = NULL;
        __atomic_store_n(&FPU_nm_hits[cpu_index], 0, __ATOMIC_RELAXED);
    }

    FPU_sse_enabled = true;

    kdebug_printf("[FPU] cpu=%u init sse=on avx=%s lazy=#NM state=%uB mode=%s\n",
                  cpu_index,
                  FPU_avx_enabled ? "on" : "off",
                  FPU_state_area_size,
                  FPU_avx_enabled ? "xsave" : "fxsave");
    return true;
}

void FPU_lazy_on_task_switch(void)
{
    if (!FPU_sse_enabled)
        return;

    x86_set_ts();
}

void FPU_lazy_probe_current_cpu(void)
{
    if (!FPU_sse_enabled)
        return;

    __asm__ __volatile__("xorps %%xmm0, %%xmm0" : : : "xmm0");

    if (FPU_avx_enabled)
    {
        __asm__ __volatile__("vxorps %%ymm0, %%ymm0, %%ymm0\n\tvzeroupper" : : : "ymm0");
    }
}

bool FPU_is_sse_enabled(void)
{
    return FPU_sse_enabled;
}

bool FPU_is_avx_enabled(void)
{
    return FPU_avx_enabled;
}

bool FPU_stress_ymm_local(uint32_t iterations, uint64_t* signature_out)
{
    if (!FPU_sse_enabled || !FPU_avx_enabled || FPU_state_area_size == 0)
        return false;

    if (iterations == 0)
        iterations = 1;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        cpu_index = 0;

    void* state_a = (void*) &FPU_stress_state_a[cpu_index][0];
    void* state_b = (void*) &FPU_stress_state_b[cpu_index][0];

    memset(state_a, 0, FPU_state_area_size);
    memset(state_b, 0, FPU_state_area_size);

    uint8_t pattern_a[32] __attribute__((aligned(32)));
    uint8_t pattern_b[32] __attribute__((aligned(32)));
    uint8_t out[32] __attribute__((aligned(32)));

    uint64_t signature = 0;
    bool ok = true;

    x86_clear_ts();

    for (uint32_t iter = 0; iter < iterations; iter++)
    {
        for (uint32_t i = 0; i < sizeof(pattern_a); i++)
        {
            pattern_a[i] = (uint8_t) (((iter * 13U) + (i * 7U) + 0x11U) & 0xFFU);
            pattern_b[i] = (uint8_t) (((iter * 17U) + (i * 3U) + 0x5AU) & 0xFFU);
        }

        __asm__ __volatile__("vmovdqu %0, %%ymm0" : : "m"(*(const uint8_t (*)[32]) pattern_a) : "ymm0", "memory");
        FPU_xsave_to(state_a);

        __asm__ __volatile__("vmovdqu %0, %%ymm0" : : "m"(*(const uint8_t (*)[32]) pattern_b) : "ymm0", "memory");
        FPU_xsave_to(state_b);

        FPU_xrstor_from(state_a);
        __asm__ __volatile__("vmovdqu %%ymm0, %0" : "=m"(*(uint8_t (*)[32]) out) : : "memory");
        if (memcmp(out, pattern_a, sizeof(out)) != 0)
        {
            ok = false;
            break;
        }

        for (uint32_t i = 0; i < sizeof(out); i++)
            signature = (signature << 5) ^ (signature >> 2) ^ out[i];

        FPU_xrstor_from(state_b);
        __asm__ __volatile__("vmovdqu %%ymm0, %0" : "=m"(*(uint8_t (*)[32]) out) : : "memory");
        if (memcmp(out, pattern_b, sizeof(out)) != 0)
        {
            ok = false;
            break;
        }

        for (uint32_t i = 0; i < sizeof(out); i++)
            signature = (signature << 5) ^ (signature >> 2) ^ out[i];
    }

    __asm__ __volatile__("vzeroupper");

    if (signature_out)
        *signature_out = signature;

    x86_set_ts();
    return ok;
}

static void FPU_nm_handler(interrupt_frame_t* frame)
{
    (void) frame;

    if (!FPU_sse_enabled)
        return;

    uint32_t cpu_index = task_get_current_cpu_index();
    if (cpu_index >= TASK_MAX_CPUS)
        cpu_index = 0;

    // TS must be cleared before any FXSAVE/FXRSTOR.
    x86_clear_ts();

    task_t* current = task_get_current_task();
    task_t* owner = FPU_owner[cpu_index];

    if (!current)
    {
        __asm__ __volatile__("fninit");
        FPU_owner[cpu_index] = NULL;
        return;
    }

    if (owner == current)
    {
        uint64_t hits = __atomic_add_fetch(&FPU_nm_hits[cpu_index], 1, __ATOMIC_RELAXED);
        if (hits == 1)
            kdebug_printf("[FPU] #NM cpu=%u task=%p owner=reused\n", cpu_index, current);
        return;
    }

    if (owner && owner->fpu_initialized)
        FPU_save_task(owner);

    if (!current->fpu_initialized)
    {
        if (!FPU_ensure_task_state(current))
        {
            __asm__ __volatile__("fninit");
            FPU_owner[cpu_index] = NULL;
            return;
        }

        if (FPU_initial_state_ready)
            FPU_restore_state(FPU_initial_state);
        else
            __asm__ __volatile__("fninit");

        current->fpu_initialized = 1;
    }
    else
    {
        if (!FPU_restore_task(current))
        {
            __asm__ __volatile__("fninit");
            FPU_owner[cpu_index] = NULL;
            return;
        }
    }

    FPU_owner[cpu_index] = current;

    uint64_t hits = __atomic_add_fetch(&FPU_nm_hits[cpu_index], 1, __ATOMIC_RELAXED);
    if (hits == 1)
        kdebug_printf("[FPU] #NM cpu=%u task=%p owner=switch\n", cpu_index, current);
}
