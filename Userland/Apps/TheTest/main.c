#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <libc_tls.h>
#include <sched.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <drm/drm_mode.h>
#include <dlfcn.h>
#include <linux/soundcard.h>

#define SYS_UNDEFINED_TEST 9999L
#define KERNEL_TEST_ADDR   0xFFFFFFFF80100000ULL
#define MMAP_TEST_LEN      0x1000ULL
#define USER_MMAP_BASE     0x0000000050000000ULL
#define USER_MMAP_LIMIT    0x000000006F000000ULL
#define USER_MMAP_WINDOW_LEN (USER_MMAP_LIMIT - USER_MMAP_BASE)
#define MMAP_LEN_64MIB     0x04000000ULL
#define UNMAP_TEST_ADDR    0x0000000060000000ULL
#define RACE_FILE_PATH     "/race_counter.txt"
#define RACE_WORKERS       4
#define RACE_ITERS         64
#define RACE_WAIT_TIMEOUT_MS 15000

#define TEST_UNDEFINED_SYSCALL
#define TEST_MMAP_FORBIDDEN
#define TEST_MMAP_LEN_EDGECASES
#define TEST_UNMAP_EDGECASES
#define TEST_RACE_CONCURRENCY
#define TEST_HEAP_STRESS
// Experimental scheduler probe: expects timer-driven process preemption.
#define TEST_PREEMPTIVE_TIMER
#define TEST_COW_FORK
// pthread API is expected to be true shared-address-space threading.
#define TEST_PTHREAD_SHIM
#define TEST_TLS_DYNAMIC
#define TEST_DRM_KMS
#define TEST_AUDIO_DSP
#define TEST_LIBDL
// #define TEST_READ_KERNEL
// #define TEST_WRITE_KERNEL

#define HEAP_STRESS_SLOTS      64U
#define HEAP_STRESS_ITERS      6000U
#define HEAP_STRESS_MAX_ALLOC  4096U
#define HEAP_STRESS_CHECK_SPAN 64U

#define THETEST_DRM_DUMB_MAX_BYTES    (32U * 1024U * 1024U)
#define THETEST_AUDIO_TONE_FREQ_HZ    880U
#define THETEST_AUDIO_TONE_DURATION_MS 200U

static inline uint64_t thetest_rdtsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | (uint64_t) lo;
}

static uint64_t thetest_tsc_cycles_per_ms(void)
{
    const uint32_t sample_ms = 20U;
    uint64_t start = thetest_rdtsc();
    (void) usleep(sample_ms * 1000U);
    uint64_t end = thetest_rdtsc();
    if (end <= start)
        return 0;

    return (end - start) / (uint64_t) sample_ms;
}

static void thetest_mmap_probe(const char* label, uintptr_t addr, size_t len, bool expect_fail)
{
    printf("[TheTest] mmap test '%s': addr=%p len=0x%llx\n",
           label,
           (void*) addr,
           (unsigned long long) len);

    void* map_ptr = mmap((void*) addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_ptr == MAP_FAILED)
    {
        printf("[TheTest] mmap %s\n", expect_fail ? "refused (expected)" : "failed (unexpected)");
        return;
    }

    printf("[TheTest] mmap %s ptr=%p\n",
           expect_fail ? "succeeded (unexpected)" : "succeeded",
           map_ptr);

    int unmap_rc = munmap(map_ptr, len);
    printf("[TheTest] unmap rc=%d\n", unmap_rc);
}

static void thetest_unmap_probe(void)
{
    printf("[TheTest] unmap test 'unmapped zone': addr=%p len=0x%llx\n",
           (void*) (uintptr_t) UNMAP_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    int unmapped_rc = munmap((void*) (uintptr_t) UNMAP_TEST_ADDR, (size_t) MMAP_TEST_LEN);
    printf("[TheTest] unmap on unmapped zone rc=%d (expected<0)\n", unmapped_rc);

    printf("[TheTest] unmap test 'double unmap': map addr=%p len=0x%llx\n",
           (void*) (uintptr_t) UNMAP_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    void* map_ptr = mmap((void*) (uintptr_t) UNMAP_TEST_ADDR,
                         (size_t) MMAP_TEST_LEN,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);
    if (map_ptr == MAP_FAILED)
    {
        printf("[TheTest] setup map failed (unexpected)\n");
        return;
    }

    int first_unmap_rc = munmap(map_ptr, (size_t) MMAP_TEST_LEN);
    int second_unmap_rc = munmap(map_ptr, (size_t) MMAP_TEST_LEN);
    printf("[TheTest] first unmap rc=%d (expected=0)\n", first_unmap_rc);
    printf("[TheTest] second unmap rc=%d (expected<0)\n", second_unmap_rc);
}

static bool thetest_parse_u64(const char* text, uint64_t* out_value)
{
    if (!text || !out_value)
        return false;

    uint64_t value = 0;
    bool has_digit = false;
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        char c = text[i];
        if (c >= '0' && c <= '9')
        {
            has_digit = true;
            value = (value * 10ULL) + (uint64_t) (c - '0');
            continue;
        }

        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;

        return false;
    }

    if (!has_digit)
        return false;

    *out_value = value;
    return true;
}

static bool thetest_read_counter(const char* path, uint64_t* out_value)
{
    if (!path || !out_value)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t raw[64];
    int read_rc = (int) read(fd, raw, sizeof(raw) - 1U);
    (void) close(fd);
    if (read_rc < 0)
        return false;
    size_t out_size = (size_t) read_rc;

    raw[out_size] = '\0';
    return thetest_parse_u64((const char*) raw, out_value);
}

static bool thetest_write_counter(const char* path, uint64_t value)
{
    if (!path)
        return false;

    char text[64];
    int text_len = snprintf(text, sizeof(text), "%llu\n", (unsigned long long) value);
    if (text_len <= 0 || (size_t) text_len >= sizeof(text))
        return false;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;

    int write_rc = (int) write(fd, text, (size_t) text_len);
    int close_rc = close(fd);
    return write_rc == text_len && close_rc == 0;
}

static bool thetest_increment_counter_atomic(const char* path)
{
    if (!path)
        return false;

    for (uint32_t attempt = 0; attempt < 8192U; attempt++)
    {
        int fd = open(path, O_RDWR | O_CREAT | O_LOCK);
        if (fd < 0)
        {
            (void) sched_yield();
            continue;
        }

        uint8_t raw[64];
        int read_rc = (int) read(fd, raw, sizeof(raw) - 1U);
        if (read_rc < 0)
        {
            (void) close(fd);
            return false;
        }

        uint64_t current = 0;
        if (read_rc > 0)
        {
            raw[(size_t) read_rc] = '\0';
            if (!thetest_parse_u64((const char*) raw, &current))
            {
                (void) close(fd);
                return false;
            }
        }

        uint64_t next = current + 1ULL;
        char text[64];
        int text_len = snprintf(text, sizeof(text), "%llu\n", (unsigned long long) next);
        if (text_len <= 0 || (size_t) text_len >= sizeof(text))
        {
            (void) close(fd);
            return false;
        }

        if (lseek(fd, 0, SEEK_SET) < 0)
        {
            (void) close(fd);
            return false;
        }

        int write_rc = (int) write(fd, text, (size_t) text_len);
        int close_rc = close(fd);
        return write_rc == text_len && close_rc == 0;
    }

    return false;
}

static int thetest_wait_child(int pid, int* out_status, int* out_signal, uint32_t timeout_ms)
{
    if (out_status)
        *out_status = 0;
    if (out_signal)
        *out_signal = 0;

    uint32_t waited_ms = 0;
    for (;;)
    {
        int wait_status = 0;
        int rc = waitpid(pid, &wait_status, WNOHANG);
        if (rc == pid)
        {
            if (out_status)
                *out_status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 0;
            if (out_signal)
                *out_signal = WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
            return rc;
        }

        if (rc < 0)
            return rc;

        (void) usleep(1000U);
        waited_ms++;
        if (timeout_ms != 0U && waited_ms >= timeout_ms)
            return -2;
    }
}

static void thetest_race_worker(const char* path, int worker_id)
{
    for (int i = 0; i < RACE_ITERS; i++)
    {
        if (!thetest_increment_counter_atomic(path))
        {
            printf("[TheTest][worker:%d] atomic increment failed iter=%d\n", worker_id, i);
            _exit(3);
        }

        if ((i & 3) == 0)
            (void) sched_yield();
    }

    _exit(0);
}

static void thetest_race_probe(void)
{
    printf("[TheTest] race test: workers=%d iters=%d file=%s\n",
           RACE_WORKERS,
           RACE_ITERS,
           RACE_FILE_PATH);

    if (!thetest_write_counter(RACE_FILE_PATH, 0))
    {
        printf("[TheTest] race setup failed: cannot init counter file\n");
        return;
    }

    int pids[RACE_WORKERS];
    for (int i = 0; i < RACE_WORKERS; i++)
        pids[i] = -1;

    for (int i = 0; i < RACE_WORKERS; i++)
    {
        int pid = fork();
        if (pid < 0)
        {
            printf("[TheTest] race fork failed at worker=%d rc=%d\n", i, pid);
            break;
        }

        if (pid == 0)
            thetest_race_worker(RACE_FILE_PATH, i);

        pids[i] = pid;
    }

    for (int i = 0; i < RACE_WORKERS; i++)
    {
        if (pids[i] <= 0)
            continue;

        int status = 0;
        int signal = 0;
        int wait_rc = thetest_wait_child(pids[i], &status, &signal, RACE_WAIT_TIMEOUT_MS);
        if (wait_rc < 0)
        {
            if (wait_rc == -2)
                printf("[TheTest] race wait timeout pid=%d after %u ms\n",
                       pids[i],
                       (unsigned int) RACE_WAIT_TIMEOUT_MS);
            else
                printf("[TheTest] race wait failed pid=%d rc=%d\n", pids[i], wait_rc);
        }
        else if (signal != 0)
            printf("[TheTest] race worker pid=%d killed by signal=%d\n", pids[i], signal);
        else if (status != 0)
            printf("[TheTest] race worker pid=%d exit status=%d\n", pids[i], status);
    }

    uint64_t final_counter = 0;
    if (!thetest_read_counter(RACE_FILE_PATH, &final_counter))
    {
        printf("[TheTest] race result read failed\n");
        return;
    }

    uint64_t expected = (uint64_t) RACE_WORKERS * (uint64_t) RACE_ITERS;
    printf("[TheTest] race result: expected=%llu observed=%llu\n",
           (unsigned long long) expected,
           (unsigned long long) final_counter);
    if (final_counter != expected)
        printf("[TheTest] race detected: lost updates observed\n");
    else
        printf("[TheTest] race not observed this run\n");
}

typedef struct thetest_heap_slot
{
    uint8_t* ptr;
    size_t size;
    uint32_t pattern_seed;
} thetest_heap_slot_t;

static uint32_t thetest_lcg_next(uint32_t* state)
{
    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

static uint8_t thetest_heap_pattern(uint32_t seed, size_t index)
{
    return (uint8_t) (((seed * 33U) + ((uint32_t) index * 17U)) & 0xFFU);
}

static void thetest_heap_fill(const thetest_heap_slot_t* slot)
{
    if (!slot || !slot->ptr)
        return;

    for (size_t i = 0; i < slot->size; i++)
        slot->ptr[i] = thetest_heap_pattern(slot->pattern_seed, i);
}

static bool thetest_heap_verify_region(const thetest_heap_slot_t* slot, size_t begin, size_t end)
{
    if (!slot || !slot->ptr)
        return true;

    if (end > slot->size)
        end = slot->size;
    if (begin > end)
        begin = end;

    for (size_t i = begin; i < end; i++)
    {
        uint8_t expected = thetest_heap_pattern(slot->pattern_seed, i);
        if (slot->ptr[i] != expected)
        {
            printf("[TheTest][heap] corruption ptr=%p idx=%llu got=0x%02x expected=0x%02x\n",
                   (void*) slot->ptr,
                   (unsigned long long) i,
                   (unsigned int) slot->ptr[i],
                   (unsigned int) expected);
            return false;
        }
    }

    return true;
}

static bool thetest_heap_verify_slot(const thetest_heap_slot_t* slot)
{
    if (!slot || !slot->ptr)
        return true;

    size_t head = (slot->size < HEAP_STRESS_CHECK_SPAN) ? slot->size : HEAP_STRESS_CHECK_SPAN;
    if (!thetest_heap_verify_region(slot, 0U, head))
        return false;

    if (slot->size > HEAP_STRESS_CHECK_SPAN)
    {
        size_t tail_begin = slot->size - HEAP_STRESS_CHECK_SPAN;
        if (!thetest_heap_verify_region(slot, tail_begin, slot->size))
            return false;
    }

    return true;
}

static bool thetest_heap_alloc_slot(thetest_heap_slot_t* slot, uint32_t* rng_state)
{
    if (!slot || !rng_state || slot->ptr)
        return false;

    uint32_t pick = thetest_lcg_next(rng_state);
    size_t size = (size_t) (pick % HEAP_STRESS_MAX_ALLOC) + 1U;
    uint32_t mode = (pick >> 8U) & 0x3U;

    void* ptr = NULL;
    if (mode == 0U)
    {
        ptr = malloc(size);
    }
    else if (mode == 1U)
    {
        ptr = calloc(size, 1U);
        if (ptr)
        {
            uint8_t* bytes = (uint8_t*) ptr;
            size_t check = (size < HEAP_STRESS_CHECK_SPAN) ? size : HEAP_STRESS_CHECK_SPAN;
            for (size_t i = 0; i < check; i++)
            {
                if (bytes[i] != 0U)
                {
                    printf("[TheTest][heap] calloc non-zero ptr=%p idx=%llu val=0x%02x\n",
                           ptr,
                           (unsigned long long) i,
                           (unsigned int) bytes[i]);
                    free(ptr);
                    return false;
                }
            }
        }
    }
    else if (mode == 2U)
    {
        size_t alignment = (size_t) 1U << (4U + ((pick >> 16U) & 0x7U));
        int rc = posix_memalign(&ptr, alignment, size);
        if (rc != 0)
        {
            printf("[TheTest][heap] posix_memalign failed rc=%d align=%llu size=%llu\n",
                   rc,
                   (unsigned long long) alignment,
                   (unsigned long long) size);
            return false;
        }
        if (((uintptr_t) ptr & (alignment - 1U)) != 0U)
        {
            printf("[TheTest][heap] posix_memalign bad alignment ptr=%p align=%llu\n",
                   ptr,
                   (unsigned long long) alignment);
            free(ptr);
            return false;
        }
    }
    else
    {
        size_t alignment = (size_t) 1U << (4U + ((pick >> 20U) & 0x7U));
        size_t aligned_size = (size + (alignment - 1U)) & ~(alignment - 1U);
        ptr = aligned_alloc(alignment, aligned_size);
        size = aligned_size;
        if (ptr == NULL)
        {
            printf("[TheTest][heap] aligned_alloc failed align=%llu size=%llu errno=%d\n",
                   (unsigned long long) alignment,
                   (unsigned long long) aligned_size,
                   errno);
            return false;
        }
        if (((uintptr_t) ptr & (alignment - 1U)) != 0U)
        {
            printf("[TheTest][heap] aligned_alloc bad alignment ptr=%p align=%llu\n",
                   ptr,
                   (unsigned long long) alignment);
            free(ptr);
            return false;
        }
    }

    if (!ptr)
    {
        printf("[TheTest][heap] allocation failed mode=%u size=%llu errno=%d\n",
               (unsigned int) mode,
               (unsigned long long) size,
               errno);
        return false;
    }

    slot->ptr = (uint8_t*) ptr;
    slot->size = size;
    slot->pattern_seed = thetest_lcg_next(rng_state);
    thetest_heap_fill(slot);
    return true;
}

static bool thetest_heap_realloc_slot(thetest_heap_slot_t* slot, uint32_t* rng_state)
{
    if (!slot || !rng_state || !slot->ptr)
        return false;

    if (!thetest_heap_verify_slot(slot))
        return false;

    size_t new_size = (size_t) ((thetest_lcg_next(rng_state) % HEAP_STRESS_MAX_ALLOC) + 1U);
    uint8_t* old_ptr = slot->ptr;
    size_t old_size = slot->size;
    uint32_t old_seed = slot->pattern_seed;

    void* new_ptr = realloc(slot->ptr, new_size);
    if (!new_ptr)
    {
        printf("[TheTest][heap] realloc failed old=%p old_size=%llu new_size=%llu errno=%d\n",
               (void*) old_ptr,
               (unsigned long long) old_size,
               (unsigned long long) new_size,
               errno);
        return false;
    }

    size_t preserved = (old_size < new_size) ? old_size : new_size;
    thetest_heap_slot_t preserved_view = {
        .ptr = (uint8_t*) new_ptr,
        .size = preserved,
        .pattern_seed = old_seed,
    };
    if (!thetest_heap_verify_slot(&preserved_view))
        return false;

    slot->ptr = (uint8_t*) new_ptr;
    slot->size = new_size;
    slot->pattern_seed = thetest_lcg_next(rng_state);
    thetest_heap_fill(slot);
    return true;
}

static bool thetest_heap_free_slot(thetest_heap_slot_t* slot)
{
    if (!slot || !slot->ptr)
        return true;

    if (!thetest_heap_verify_slot(slot))
        return false;

    free(slot->ptr);
    slot->ptr = NULL;
    slot->size = 0U;
    slot->pattern_seed = 0U;
    return true;
}

static bool thetest_heap_api_edges(void)
{
    void* sentinel = (void*) (uintptr_t) 0x1234U;
    void* ptr = sentinel;

    int rc = posix_memalign(&ptr, 24U, 128U);
    if (rc != EINVAL || ptr != sentinel)
    {
        printf("[TheTest][heap] posix_memalign invalid alignment rc=%d ptr=%p expected_ptr=%p\n",
               rc,
               ptr,
               sentinel);
        return false;
    }

    errno = 0;
    void* bad = aligned_alloc(64U, 100U);
    if (bad != NULL || errno != EINVAL)
    {
        printf("[TheTest][heap] aligned_alloc invalid size ptr=%p errno=%d expected=%d\n",
               bad,
               errno,
               EINVAL);
        if (bad)
            free(bad);
        return false;
    }

    return true;
}

static void thetest_heap_stress_probe(void)
{
    printf("[TheTest] heap stress start: slots=%u iters=%u max_alloc=%u\n",
           (unsigned int) HEAP_STRESS_SLOTS,
           (unsigned int) HEAP_STRESS_ITERS,
           (unsigned int) HEAP_STRESS_MAX_ALLOC);

    if (!thetest_heap_api_edges())
    {
        printf("[TheTest] heap stress failed during API edge checks\n");
        return;
    }

    thetest_heap_slot_t slots[HEAP_STRESS_SLOTS];
    memset(slots, 0, sizeof(slots));

    uint32_t rng_state = 0xC0FFEE11U;
    for (size_t i = 0; i < HEAP_STRESS_ITERS; i++)
    {
        size_t index = (size_t) (thetest_lcg_next(&rng_state) % HEAP_STRESS_SLOTS);
        uint32_t action = thetest_lcg_next(&rng_state) % 100U;

        bool ok = true;
        if (!slots[index].ptr)
            ok = thetest_heap_alloc_slot(&slots[index], &rng_state);
        else if (action < 35U)
            ok = thetest_heap_free_slot(&slots[index]);
        else if (action < 80U)
            ok = thetest_heap_realloc_slot(&slots[index], &rng_state);
        else
            ok = thetest_heap_verify_slot(&slots[index]);

        if (!ok)
        {
            printf("[TheTest] heap stress failed at iter=%llu slot=%llu action=%u\n",
                   (unsigned long long) i,
                   (unsigned long long) index,
                   (unsigned int) action);
            break;
        }

        if ((i % 17U) == 0U)
        {
            size_t extra = (size_t) (thetest_lcg_next(&rng_state) % HEAP_STRESS_SLOTS);
            if (slots[extra].ptr)
                (void) thetest_heap_free_slot(&slots[extra]);
        }
    }

    bool all_ok = true;
    for (size_t i = 0; i < HEAP_STRESS_SLOTS; i++)
    {
        if (!thetest_heap_free_slot(&slots[i]))
            all_ok = false;
    }

    if (all_ok)
        printf("[TheTest] heap stress done: OK\n");
    else
        printf("[TheTest] heap stress done: FAILED\n");
}

static void thetest_preemptive_probe(void)
{
    const uint32_t fallback_spin_iters = 25000000U;
    const uint32_t child_busy_target_ms = 250U;
    const uint32_t probe_window_ms = 1500U;
    const uint32_t settle_timeout_ms = 5000U;
    uint64_t cycles_per_ms = thetest_tsc_cycles_per_ms();
    uint64_t spin_cycles = 0;
    if (cycles_per_ms != 0 && child_busy_target_ms <= (UINT64_MAX / cycles_per_ms))
        spin_cycles = cycles_per_ms * (uint64_t) child_busy_target_ms;

    int pid = fork();
    if (pid < 0)
    {
        printf("[TheTest] preemptive test failed: fork rc=%d\n", pid);
        return;
    }

    if (pid == 0)
    {
        volatile uint64_t acc = 0x123456789ABCDEF0ULL;
        if (spin_cycles != 0)
        {
            uint64_t start = thetest_rdtsc();
            while ((thetest_rdtsc() - start) < spin_cycles)
                acc = ((acc << 7U) ^ (acc >> 3U)) + 0x9E3779B97F4A7C15ULL;
        }
        else
        {
            for (uint32_t i = 0; i < fallback_spin_iters; i++)
                acc = ((acc << 7U) ^ (acc >> 3U)) + (uint64_t) i;
        }

        if (acc == 0ULL)
            _exit(2);
        _exit(0);
    }

    bool observed_parent_running = false;
    bool child_reaped = false;
    int status = 0;
    int signal = 0;
    int wait_rc = 0;

    for (uint32_t waited = 0; waited < probe_window_ms; waited++)
    {
        int wait_status = 0;
        int rc = waitpid(pid, &wait_status, WNOHANG);
        if (rc == pid)
        {
            child_reaped = true;
            status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 0;
            signal = WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
            break;
        }

        if (rc < 0)
        {
            wait_rc = rc;
            break;
        }

        observed_parent_running = true;
        (void) usleep(1000U);
    }

    if (!child_reaped && wait_rc == 0)
    {
        wait_rc = thetest_wait_child(pid, &status, &signal, settle_timeout_ms);
        child_reaped = (wait_rc == pid);
    }

    if (wait_rc < 0 && !child_reaped)
    {
        if (wait_rc == -2)
            printf("[TheTest] preemptive test: timeout after %u ms\n", (unsigned int) settle_timeout_ms);
        else
            printf("[TheTest] preemptive test: wait failed rc=%d\n", wait_rc);
        return;
    }

    if (signal != 0 || status != 0)
    {
        printf("[TheTest] preemptive test: child failed status=%d signal=%d\n", status, signal);
        return;
    }

    if (observed_parent_running)
        printf("[TheTest] preemptive timer test: OK\n");
    else
        printf("[TheTest] preemptive timer test: FAILED (child monopolized CPU)\n");
}

static void thetest_cow_probe(void)
{
    const size_t probe_size = 4096U;
    uint8_t* probe = (uint8_t*) malloc(probe_size);
    if (!probe)
    {
        printf("[TheTest] COW test failed: malloc(%llu)\n", (unsigned long long) probe_size);
        return;
    }

    memset(probe, 0x11, probe_size);
    probe[0] = 0xA1;
    probe[probe_size - 1U] = 0xB2;

    int pid = fork();
    if (pid < 0)
    {
        printf("[TheTest] COW test failed: fork rc=%d\n", pid);
        free(probe);
        return;
    }

    if (pid == 0)
    {
        if (probe[0] != 0xA1 || probe[probe_size - 1U] != 0xB2)
            _exit(2);

        probe[0] = 0xCC;
        probe[probe_size - 1U] = 0xDD;
        if (probe[0] != 0xCC || probe[probe_size - 1U] != 0xDD)
            _exit(3);
        _exit(0);
    }

    int status = 0;
    int signal = 0;
    int wait_rc = thetest_wait_child(pid, &status, &signal, 5000U);
    if (wait_rc < 0)
    {
        printf("[TheTest] COW test wait failed pid=%d rc=%d\n", pid, wait_rc);
        free(probe);
        return;
    }
    if (signal != 0 || status != 0)
    {
        printf("[TheTest] COW test child failed pid=%d status=%d signal=%d\n", pid, status, signal);
        free(probe);
        return;
    }

    if (probe[0] == 0xA1 && probe[probe_size - 1U] == 0xB2)
        printf("[TheTest] COW fork test: OK\n");
    else
        printf("[TheTest] COW fork test: FAILED parent modified (%u,%u)\n",
               (unsigned int) probe[0],
               (unsigned int) probe[probe_size - 1U]);

    free(probe);
}

typedef struct thetest_pthread_arg
{
    int id;
    pthread_mutex_t* lock;
    volatile int* shared_counter;
} thetest_pthread_arg_t;

static void* thetest_pthread_worker(void* arg)
{
    thetest_pthread_arg_t* cfg = (thetest_pthread_arg_t*) arg;
    volatile uint64_t acc = 0;
    for (uint32_t i = 0; i < 250000U; i++)
        acc = (acc * 1664525ULL) + (uint64_t) i + (uint64_t) cfg->id;
    (void) acc;

    (void) pthread_mutex_lock(cfg->lock);
    *cfg->shared_counter += 1;
    (void) pthread_mutex_unlock(cfg->lock);

    return (void*) (uintptr_t) (cfg->id + 1);
}

static void thetest_pthread_probe(void)
{
    const int worker_count = 4;
    pthread_t threads[4];
    thetest_pthread_arg_t args[4];
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    volatile int shared_counter = 0;
    int created = 0;

    for (int i = 0; i < worker_count; i++)
    {
        args[i].id = i;
        args[i].lock = &lock;
        args[i].shared_counter = &shared_counter;
        int rc = pthread_create(&threads[i], NULL, thetest_pthread_worker, &args[i]);
        if (rc != 0)
        {
            printf("[TheTest] pthread_create failed idx=%d rc=%d\n", i, rc);
            break;
        }
        created++;
    }

    int sum = 0;
    bool ok = (created == worker_count);
    for (int i = 0; i < created; i++)
    {
        void* retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        if (rc != 0)
        {
            ok = false;
            printf("[TheTest] pthread_join failed idx=%d rc=%d\n", i, rc);
            continue;
        }

        sum += (int) (uintptr_t) retval;
    }

    int expected = (worker_count * (worker_count + 1)) / 2;
    if (ok && sum == expected && shared_counter == worker_count)
        printf("[TheTest] pthread shared-memory test: OK (sum=%d counter=%d)\n", sum, (int) shared_counter);
    else
        printf("[TheTest] pthread shared-memory test: FAILED (sum=%d expected=%d counter=%d created=%d)\n",
               sum,
               expected,
               (int) shared_counter,
               created);

    (void) pthread_mutex_destroy(&lock);
}

typedef struct thetest_tls_dynamic_arg
{
    size_t module_id;
    uint64_t expected_init;
    uint64_t tag;
} thetest_tls_dynamic_arg_t;

static void* thetest_tls_dynamic_worker(void* arg)
{
    thetest_tls_dynamic_arg_t* cfg = (thetest_tls_dynamic_arg_t*) arg;
    libc_tls_index_t index = {
        .module_id = (uint64_t) cfg->module_id,
        .offset = 0
    };

    uint64_t* slot = (uint64_t*) __tls_get_addr(&index);
    if (!slot)
        return (void*) (uintptr_t) 1U;
    if (*slot != cfg->expected_init)
        return (void*) (uintptr_t) 2U;

    *slot = cfg->tag;
    for (uint32_t i = 0; i < 10000U; i++)
        (void) sched_yield();

    return (*slot == cfg->tag) ? (void*) (uintptr_t) 0U : (void*) (uintptr_t) 3U;
}

static void thetest_tls_dynamic_probe(void)
{
    const uint64_t init_value = 0x1122334455667788ULL;
    const uint64_t main_value = 0xAABBCCDDEEFF0011ULL;
    size_t module_id = 0;

    if (__libc_tls_module_register(&init_value,
                                   sizeof(init_value),
                                   sizeof(init_value),
                                   16U,
                                   &module_id) < 0)
    {
        printf("[TheTest] tls dynamic register failed errno=%d\n", errno);
        return;
    }

    libc_tls_index_t index = {
        .module_id = (uint64_t) module_id,
        .offset = 0
    };
    uint64_t* main_slot = (uint64_t*) __tls_get_addr(&index);
    if (!main_slot)
    {
        printf("[TheTest] tls dynamic main slot failed\n");
        (void) __libc_tls_module_unregister(module_id);
        return;
    }
    if (*main_slot != init_value)
    {
        printf("[TheTest] tls dynamic init mismatch got=0x%llx expected=0x%llx\n",
               (unsigned long long) *main_slot,
               (unsigned long long) init_value);
        (void) __libc_tls_module_unregister(module_id);
        return;
    }
    *main_slot = main_value;

    const int worker_count = 4;
    pthread_t threads[4];
    thetest_tls_dynamic_arg_t args[4];
    int created = 0;
    bool ok = true;

    for (int i = 0; i < worker_count; i++)
    {
        args[i].module_id = module_id;
        args[i].expected_init = init_value;
        args[i].tag = 0xABC00000ULL + (uint64_t) i;

        int rc = pthread_create(&threads[i], NULL, thetest_tls_dynamic_worker, &args[i]);
        if (rc != 0)
        {
            printf("[TheTest] tls dynamic pthread_create failed idx=%d rc=%d\n", i, rc);
            ok = false;
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++)
    {
        void* retval = NULL;
        int rc = pthread_join(threads[i], &retval);
        if (rc != 0 || (uintptr_t) retval != 0U)
        {
            printf("[TheTest] tls dynamic worker failed idx=%d rc=%d retval=%llu\n",
                   i,
                   rc,
                   (unsigned long long) (uintptr_t) retval);
            ok = false;
        }
    }

    if (*main_slot != main_value)
    {
        printf("[TheTest] tls dynamic main contamination got=0x%llx expected=0x%llx\n",
               (unsigned long long) *main_slot,
               (unsigned long long) main_value);
        ok = false;
    }

    if (__libc_tls_module_unregister(module_id) < 0)
    {
        printf("[TheTest] tls dynamic unregister failed errno=%d\n", errno);
        ok = false;
    }

    for (uint32_t i = 0; i < (LIBC_TLS_MAX_MODULES * 4U); i++)
    {
        size_t churn_id = 0;
        if (__libc_tls_module_register(&init_value,
                                       sizeof(init_value),
                                       sizeof(init_value),
                                       16U,
                                       &churn_id) < 0)
        {
            printf("[TheTest] tls dynamic churn register failed iter=%u errno=%d\n",
                   i,
                   errno);
            ok = false;
            break;
        }

        if (__libc_tls_module_unregister(churn_id) < 0)
        {
            printf("[TheTest] tls dynamic churn unregister failed iter=%u errno=%d\n",
                   i,
                   errno);
            ok = false;
            break;
        }
    }

    if (ok && created == worker_count)
        printf("[TheTest] tls dynamic module test: OK (module=%llu)\n",
               (unsigned long long) module_id);
    else
        printf("[TheTest] tls dynamic module test: FAILED (module=%llu created=%d)\n",
               (unsigned long long) module_id,
               created);
}

static uint32_t thetest_align_up_u32(uint32_t value, uint32_t align)
{
    if (align == 0U)
        return value;

    uint32_t rem = value % align;
    if (rem == 0U)
        return value;
    return value + (align - rem);
}

static uint64_t thetest_drm_dumb_size_bytes(uint32_t width, uint32_t height)
{
    if (width == 0U || height == 0U)
        return 0ULL;

    uint64_t pitch = (uint64_t) thetest_align_up_u32(width * 4U, 64U);
    return pitch * (uint64_t) height;
}

static bool thetest_drm_mode_fits_dumb(const drm_mode_modeinfo_t* mode)
{
    if (!mode || mode->hdisplay == 0U || mode->vdisplay == 0U)
        return false;

    return thetest_drm_dumb_size_bytes(mode->hdisplay, mode->vdisplay) <= THETEST_DRM_DUMB_MAX_BYTES;
}

static bool thetest_drm_pick_mode(const drm_mode_get_resources_t* resources,
                                  const drm_mode_get_crtc_t* crtc,
                                  const drm_mode_modeinfo_t* connector_modes,
                                  uint32_t connector_mode_count,
                                  drm_mode_modeinfo_t* out_mode)
{
    if (!resources || !out_mode)
        return false;

    if (crtc && crtc->mode_valid != 0U && thetest_drm_mode_fits_dumb(&crtc->mode))
    {
        *out_mode = crtc->mode;
        return true;
    }

    uint64_t best_score = 0ULL;
    bool found = false;
    for (uint32_t i = 0; i < connector_mode_count; i++)
    {
        const drm_mode_modeinfo_t* mode = &connector_modes[i];
        if (!thetest_drm_mode_fits_dumb(mode))
            continue;

        uint64_t score = ((uint64_t) mode->hdisplay * (uint64_t) mode->vdisplay) * 1000ULL +
                         (uint64_t) mode->vrefresh;
        if (!found || score > best_score)
        {
            *out_mode = *mode;
            best_score = score;
            found = true;
        }
    }

    if (found)
        return true;

    uint32_t width = resources->max_width ? resources->max_width : 1280U;
    uint32_t height = resources->max_height ? resources->max_height : 800U;
    while (width >= 16U && height >= 16U)
    {
        drm_mode_modeinfo_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.hdisplay = (uint16_t) width;
        candidate.vdisplay = (uint16_t) height;
        candidate.vrefresh = 60U;
        if (thetest_drm_mode_fits_dumb(&candidate))
        {
            *out_mode = candidate;
            return true;
        }
        width /= 2U;
        height /= 2U;
    }

    return false;
}

static void thetest_drm_fill_gradient(uint8_t* bytes, uint32_t width, uint32_t height, uint32_t pitch)
{
    if (!bytes || width == 0U || height == 0U || pitch < width * 4U)
        return;

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t r = (uint8_t) ((x * 255U) / width);
            uint8_t g = (uint8_t) ((y * 255U) / height);
            uint8_t b = (uint8_t) (((x ^ y) * 255U) / (width > height ? width : height));
            size_t off = (size_t) y * pitch + ((size_t) x * 4U);
            bytes[off + 0U] = b;
            bytes[off + 1U] = g;
            bytes[off + 2U] = r;
            bytes[off + 3U] = 0x00U;
        }
    }
}

static bool thetest_write_full(int fd, const void* data, size_t size, size_t* out_written)
{
    if (out_written)
        *out_written = 0U;
    if (fd < 0 || !data)
        return false;

    size_t written = 0U;
    while (written < size)
    {
        int rc = (int) write(fd, (const uint8_t*) data + written, size - written);
        if (rc <= 0)
            break;
        written += (size_t) rc;
    }

    if (out_written)
        *out_written = written;
    return written == size;
}

static bool thetest_audio_emit_square_tone(int fd,
                                           uint32_t sample_rate,
                                           uint32_t channels,
                                           uint32_t format,
                                           size_t* out_bytes_written)
{
    if (out_bytes_written)
        *out_bytes_written = 0U;
    if (fd < 0 || sample_rate == 0U || (channels != 1U && channels != 2U))
        return false;

    size_t frame_count = ((size_t) sample_rate * THETEST_AUDIO_TONE_DURATION_MS) / 1000U;
    if (frame_count < 64U)
        frame_count = 64U;

    uint32_t phase = 0U;
    uint32_t step = (uint32_t) ((((uint64_t) THETEST_AUDIO_TONE_FREQ_HZ) << 32) / sample_rate);

    if (format == AFMT_S16_LE)
    {
        size_t sample_count = frame_count * channels;
        int16_t* pcm = (int16_t*) malloc(sample_count * sizeof(int16_t));
        if (!pcm)
            return false;

        for (size_t i = 0; i < frame_count; i++)
        {
            phase += step;
            int16_t sample = (phase & 0x80000000U) ? 10000 : -10000;
            for (uint32_t ch = 0U; ch < channels; ch++)
                pcm[i * channels + ch] = sample;
        }

        bool ok = thetest_write_full(fd, pcm, sample_count * sizeof(int16_t), out_bytes_written);
        free(pcm);
        return ok;
    }

    if (format == AFMT_U8)
    {
        size_t sample_count = frame_count * channels;
        uint8_t* pcm = (uint8_t*) malloc(sample_count);
        if (!pcm)
            return false;

        for (size_t i = 0; i < frame_count; i++)
        {
            phase += step;
            uint8_t sample = (phase & 0x80000000U) ? 224U : 32U;
            for (uint32_t ch = 0U; ch < channels; ch++)
                pcm[i * channels + ch] = sample;
        }

        bool ok = thetest_write_full(fd, pcm, sample_count, out_bytes_written);
        free(pcm);
        return ok;
    }

    return false;
}

static void thetest_audio_dsp_probe(void)
{
    const char* paths[] = { "/dev/dsp", "/dev/audio" };
    int dsp_fd = -1;
    const char* opened_path = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        dsp_fd = open(paths[i], O_WRONLY);
        if (dsp_fd >= 0)
        {
            opened_path = paths[i];
            break;
        }
    }

    if (dsp_fd < 0)
    {
        printf("[TheTest] audio open failed (/dev/dsp,/dev/audio) errno=%d\n", errno);
        return;
    }

    bool ok = true;

    int32_t formats = 0;
    if (ioctl(dsp_fd, SNDCTL_DSP_GETFMTS, &formats) < 0)
    {
        printf("[TheTest] audio GETFMTS failed errno=%d\n", errno);
        ok = false;
    }

    int32_t fragment = (8 << 16) | 10;
    if (ioctl(dsp_fd, SNDCTL_DSP_SETFRAGMENT, &fragment) < 0)
    {
        printf("[TheTest] audio SETFRAGMENT failed errno=%d\n", errno);
        ok = false;
    }

    int32_t speed = 11025;
    if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &speed) < 0 || speed <= 0)
    {
        printf("[TheTest] audio SPEED failed errno=%d speed=%d\n", errno, speed);
        ok = false;
    }

    int32_t stereo = 1;
    if (ioctl(dsp_fd, SNDCTL_DSP_STEREO, &stereo) < 0)
    {
        printf("[TheTest] audio STEREO(1) failed errno=%d\n", errno);
        stereo = 0;
        if (ioctl(dsp_fd, SNDCTL_DSP_STEREO, &stereo) < 0)
        {
            printf("[TheTest] audio STEREO(0) fallback failed errno=%d\n", errno);
            ok = false;
        }
    }

    int32_t format = (formats & (int32_t) AFMT_S16_LE) ? (int32_t) AFMT_S16_LE : (int32_t) AFMT_U8;
    if ((formats & ((int32_t) AFMT_S16_LE | (int32_t) AFMT_U8)) == 0)
    {
        printf("[TheTest] audio unsupported formats mask=0x%x\n", (unsigned int) formats);
        ok = false;
    }
    else if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format) < 0)
    {
        printf("[TheTest] audio SETFMT failed errno=%d\n", errno);
        ok = false;
    }

    size_t bytes_written = 0U;
    uint32_t channels = (stereo != 0) ? 2U : 1U;
    if (!thetest_audio_emit_square_tone(dsp_fd,
                                        (uint32_t) ((speed > 0) ? speed : 11025),
                                        channels,
                                        (uint32_t) format,
                                        &bytes_written))
    {
        printf("[TheTest] audio write tone failed fmt=0x%x speed=%d ch=%u errno=%d\n",
               (unsigned int) format,
               speed,
               (unsigned int) channels,
               errno);
        ok = false;
    }

    if (ioctl(dsp_fd, SNDCTL_DSP_SYNC, &format) < 0)
    {
        printf("[TheTest] audio SYNC failed errno=%d\n", errno);
        ok = false;
    }
    if (ioctl(dsp_fd, SNDCTL_DSP_RESET, &format) < 0)
    {
        printf("[TheTest] audio RESET failed errno=%d\n", errno);
        ok = false;
    }

    (void) close(dsp_fd);

    if (ok)
    {
        printf("[TheTest] audio OSS probe: OK (path=%s rate=%d ch=%u fmt=0x%x bytes=%llu fragment=0x%x)\n",
               opened_path ? opened_path : "?",
               speed,
               (unsigned int) channels,
               (unsigned int) format,
               (unsigned long long) bytes_written,
               (unsigned int) fragment);
    }
    else
    {
        printf("[TheTest] audio OSS probe: FAILED (path=%s)\n",
               opened_path ? opened_path : "?");
    }
}

static void thetest_drm_kms_probe(void)
{
    int card_fd = open(DRM_NODE_PATH, O_RDWR);
    if (card_fd < 0)
    {
        printf("[TheTest] DRM open failed path=%s errno=%d\n", DRM_NODE_PATH, errno);
        return;
    }

    bool ok = true;
    bool committed = false;

    drm_mode_get_resources_t resources;
    memset(&resources, 0, sizeof(resources));
    uint32_t connector_id = 0U;
    uint32_t crtc_id = 0U;
    uint32_t plane_id = 0U;
    resources.count_connectors = 1U;
    resources.count_crtcs = 1U;
    resources.count_planes = 1U;
    resources.connector_id_ptr = (uint64_t) (uintptr_t) &connector_id;
    resources.crtc_id_ptr = (uint64_t) (uintptr_t) &crtc_id;
    resources.plane_id_ptr = (uint64_t) (uintptr_t) &plane_id;
    if (ioctl(card_fd, DRM_IOCTL_MODE_GET_RESOURCES, &resources) < 0)
    {
        printf("[TheTest] DRM get resources failed errno=%d\n", errno);
        (void) close(card_fd);
        return;
    }

    drm_mode_get_crtc_t crtc;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    if (ioctl(card_fd, DRM_IOCTL_MODE_GET_CRTC, &crtc) < 0)
    {
        printf("[TheTest] DRM get crtc failed errno=%d\n", errno);
        ok = false;
    }

    uint32_t plane_format = 0U;
    drm_mode_get_plane_t plane;
    memset(&plane, 0, sizeof(plane));
    plane.plane_id = plane_id;
    plane.count_format_types = 1U;
    plane.format_type_ptr = (uint64_t) (uintptr_t) &plane_format;
    if (ioctl(card_fd, DRM_IOCTL_MODE_GET_PLANE, &plane) < 0)
    {
        printf("[TheTest] DRM get plane failed errno=%d\n", errno);
        ok = false;
    }
    else if (plane_format != DRM_FORMAT_XRGB8888)
    {
        printf("[TheTest] DRM unexpected primary format=0x%x (expected=0x%x)\n",
               (unsigned int) plane_format,
               (unsigned int) DRM_FORMAT_XRGB8888);
        ok = false;
    }

    drm_mode_get_connector_t connector;
    memset(&connector, 0, sizeof(connector));
    connector.connector_id = connector_id;
    if (ioctl(card_fd, DRM_IOCTL_MODE_GET_CONNECTOR, &connector) < 0)
    {
        printf("[TheTest] DRM get connector failed errno=%d\n", errno);
        (void) close(card_fd);
        return;
    }

    uint32_t connector_mode_count = connector.count_modes;
    drm_mode_modeinfo_t* connector_modes = NULL;
    if (connector_mode_count > 0U)
    {
        connector_modes = (drm_mode_modeinfo_t*) calloc(connector_mode_count, sizeof(*connector_modes));
        if (!connector_modes)
        {
            printf("[TheTest] DRM connector mode allocation failed count=%u\n",
                   (unsigned int) connector_mode_count);
            ok = false;
        }
        else
        {
            memset(&connector, 0, sizeof(connector));
            connector.connector_id = connector_id;
            connector.count_modes = connector_mode_count;
            connector.modes_ptr = (uint64_t) (uintptr_t) connector_modes;
            if (ioctl(card_fd, DRM_IOCTL_MODE_GET_CONNECTOR, &connector) < 0)
            {
                printf("[TheTest] DRM get connector modes failed errno=%d\n", errno);
                ok = false;
            }
            else
            {
                connector_mode_count = connector.count_modes;
            }
        }
    }

    drm_mode_modeinfo_t mode;
    memset(&mode, 0, sizeof(mode));
    if (!thetest_drm_pick_mode(&resources, &crtc, connector_modes, connector_mode_count, &mode))
    {
        printf("[TheTest] DRM no usable mode for dumb buffer (max=%ux%u)\n",
               (unsigned int) resources.max_width,
               (unsigned int) resources.max_height);
        ok = false;
    }

    if (connector_modes)
        free(connector_modes);
    connector_modes = NULL;

    drm_mode_create_dumb_t dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = mode.hdisplay;
    dumb.height = mode.vdisplay;
    dumb.bpp = 32U;
    if (ok && ioctl(card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0)
    {
        printf("[TheTest] DRM create dumb failed errno=%d mode=%ux%u bytes=%llu\n",
               errno,
               (unsigned int) mode.hdisplay,
               (unsigned int) mode.vdisplay,
               (unsigned long long) thetest_drm_dumb_size_bytes(mode.hdisplay, mode.vdisplay));
        ok = false;
    }

    int prime_fd = -1;
    uint32_t imported_handle = 0U;
    void* map = MAP_FAILED;
    drm_mode_create_blob_t blob;
    memset(&blob, 0, sizeof(blob));

    if (ok)
    {
        drm_prime_handle_t export_req;
        memset(&export_req, 0, sizeof(export_req));
        export_req.handle = dumb.handle;
        export_req.flags = DRM_CLOEXEC | DRM_RDWR;
        if (ioctl(card_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export_req) < 0)
        {
            printf("[TheTest] DRM handle->fd failed errno=%d\n", errno);
            ok = false;
        }
        else
        {
            prime_fd = export_req.fd;
        }
    }

    if (ok)
    {
        drm_prime_handle_t import_req;
        memset(&import_req, 0, sizeof(import_req));
        import_req.fd = prime_fd;
        import_req.flags = DRM_RDWR;
        if (ioctl(card_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import_req) < 0)
        {
            printf("[TheTest] DRM fd->handle failed errno=%d\n", errno);
            ok = false;
        }
        else
        {
            imported_handle = import_req.handle;
            if (imported_handle == 0U)
            {
                printf("[TheTest] DRM fd->handle returned zero handle\n");
                ok = false;
            }
        }
    }

    if (ok)
    {
        map = mmap(NULL, (size_t) dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, prime_fd, 0);
        if (map == MAP_FAILED)
        {
            printf("[TheTest] DRM mmap failed errno=%d size=%llu\n",
                   errno,
                   (unsigned long long) dumb.size);
            ok = false;
        }
        else
        {
            thetest_drm_fill_gradient((uint8_t*) map, dumb.width, dumb.height, dumb.pitch);
        }
    }

    if (ok)
    {
        blob.data = (uint64_t) (uintptr_t) &mode;
        blob.length = sizeof(mode);
        if (ioctl(card_fd, DRM_IOCTL_MODE_CREATE_BLOB, &blob) < 0)
        {
            printf("[TheTest] DRM create mode blob failed errno=%d\n", errno);
            ok = false;
        }
    }

    if (ok)
    {
        drm_mode_atomic_req_t atomic_test;
        memset(&atomic_test, 0, sizeof(atomic_test));
        atomic_test.flags = DRM_MODE_ATOMIC_TEST_ONLY;
        atomic_test.connector_id = connector_id;
        atomic_test.crtc_id = crtc_id;
        atomic_test.plane_id = plane_id;
        atomic_test.fb_handle = imported_handle;
        atomic_test.mode_blob_id = blob.blob_id;
        atomic_test.active = 1U;
        atomic_test.src_w = (uint32_t) mode.hdisplay << 16;
        atomic_test.src_h = (uint32_t) mode.vdisplay << 16;
        atomic_test.crtc_w = mode.hdisplay;
        atomic_test.crtc_h = mode.vdisplay;
        if (ioctl(card_fd, DRM_IOCTL_MODE_ATOMIC, &atomic_test) < 0)
        {
            printf("[TheTest] DRM atomic test-only failed errno=%d\n", errno);
            ok = false;
        }
    }

    if (ok)
    {
        drm_mode_atomic_req_t atomic_commit;
        memset(&atomic_commit, 0, sizeof(atomic_commit));
        atomic_commit.flags = 0U;
        atomic_commit.connector_id = connector_id;
        atomic_commit.crtc_id = crtc_id;
        atomic_commit.plane_id = plane_id;
        atomic_commit.fb_handle = imported_handle;
        atomic_commit.mode_blob_id = blob.blob_id;
        atomic_commit.active = 1U;
        atomic_commit.src_w = (uint32_t) mode.hdisplay << 16;
        atomic_commit.src_h = (uint32_t) mode.vdisplay << 16;
        atomic_commit.crtc_w = mode.hdisplay;
        atomic_commit.crtc_h = mode.vdisplay;
        if (ioctl(card_fd, DRM_IOCTL_MODE_ATOMIC, &atomic_commit) < 0)
        {
            printf("[TheTest] DRM atomic commit failed errno=%d\n", errno);
            ok = false;
        }
        else
        {
            committed = true;
        }
    }

    if (committed)
    {
        drm_mode_atomic_req_t atomic_disable;
        memset(&atomic_disable, 0, sizeof(atomic_disable));
        atomic_disable.connector_id = connector_id;
        atomic_disable.crtc_id = crtc_id;
        atomic_disable.plane_id = plane_id;
        atomic_disable.active = 0U;
        if (ioctl(card_fd, DRM_IOCTL_MODE_ATOMIC, &atomic_disable) < 0)
        {
            printf("[TheTest] DRM atomic disable failed errno=%d\n", errno);
            ok = false;
        }
    }

    if (blob.blob_id != 0U)
    {
        drm_mode_destroy_blob_t destroy_blob = { .blob_id = blob.blob_id };
        (void) ioctl(card_fd, DRM_IOCTL_MODE_DESTROY_BLOB, &destroy_blob);
    }
    if (map != MAP_FAILED)
        (void) munmap(map, (size_t) dumb.size);
    if (prime_fd >= 0)
        (void) close(prime_fd);
    if (dumb.handle != 0U)
    {
        drm_mode_destroy_dumb_t destroy_dumb = { .handle = dumb.handle };
        (void) ioctl(card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
    }
    (void) close(card_fd);

    if (ok)
    {
        printf("[TheTest] DRM/KMS probe: OK (mode=%ux%u pitch=%u size=%llu)\n",
               (unsigned int) mode.hdisplay,
               (unsigned int) mode.vdisplay,
               (unsigned int) dumb.pitch,
               (unsigned long long) dumb.size);
    }
    else
    {
        printf("[TheTest] DRM/KMS probe: FAILED\n");
    }
}

static void thetest_libdl_probe(void)
{
    bool ok = true;
    const char* module_path = "/lib/libthetestdyn.so";

    void* handle_global = dlopen(module_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle_global)
    {
        char* err = dlerror();
        printf("[TheTest] libdl dlopen(global) failed: %s\n", err ? err : "unknown");
        return;
    }

    void* handle_local = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle_local)
    {
        char* err = dlerror();
        printf("[TheTest] libdl dlopen(local) failed: %s\n", err ? err : "unknown");
        (void) dlclose(handle_global);
        return;
    }

    typedef uint64_t (*magic_fn_t)(uint64_t);
    typedef size_t (*len_hint_fn_t)(const char*);
    typedef int (*sum3_fn_t)(int, int, int);
    magic_fn_t magic_fn = (magic_fn_t) dlsym(handle_global, "thetestdyn_magic");
    len_hint_fn_t len_hint_fn = (len_hint_fn_t) dlsym(handle_local, "thetestdyn_len_hint");
    sum3_fn_t sum3_fn = (sum3_fn_t) dlsym(handle_local, "thetestdyn_sum3");
    magic_fn_t global_magic_fn = (magic_fn_t) dlsym(NULL, "thetestdyn_magic");
    if (!magic_fn || !len_hint_fn || !sum3_fn || !global_magic_fn)
    {
        char* err = dlerror();
        printf("[TheTest] libdl dlsym failed: %s\n", err ? err : "unknown");
        ok = false;
    }
    else
    {
        uint64_t probe = 0x0102030405060708ULL;
        uint64_t magic_local = magic_fn(probe);
        uint64_t magic_global = global_magic_fn(probe);
        size_t len = len_hint_fn("theos-libdl");
        int sum = sum3_fn(2, 3, 5);
        printf("[TheTest] libdl symbol check: magic=0x%llx len=%llu sum=%d\n",
               (unsigned long long) magic_local,
               (unsigned long long) len,
               sum);
        if (magic_local != magic_global || len != 11U || sum != 17)
            ok = false;
    }

    (void) dlsym(handle_global, "__theos_missing_symbol__");
    char* missing_err = dlerror();
    if (missing_err == NULL)
    {
        printf("[TheTest] libdl missing-symbol lookup did not report an error\n");
        ok = false;
    }

    void* missing_lib = dlopen("/lib/does-not-exist.so", RTLD_NOW);
    if (missing_lib != NULL)
    {
        printf("[TheTest] libdl missing library unexpectedly loaded\n");
        ok = false;
        (void) dlclose(missing_lib);
    }
    else
    {
        char* err = dlerror();
        if (!err)
        {
            printf("[TheTest] libdl missing library did not report an error\n");
            ok = false;
        }
    }

    for (uint32_t i = 0; i < 16U; i++)
    {
        void* h = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
        if (!h)
        {
            char* err = dlerror();
            printf("[TheTest] libdl churn dlopen failed iter=%u: %s\n",
                   (unsigned int) i,
                   err ? err : "unknown");
            ok = false;
            break;
        }

        void* sym = dlsym(h, "thetestdyn_magic");
        if (!sym)
        {
            char* err = dlerror();
            printf("[TheTest] libdl churn dlsym failed iter=%u: %s\n",
                   (unsigned int) i,
                   err ? err : "unknown");
            ok = false;
            (void) dlclose(h);
            break;
        }

        if (dlclose(h) < 0)
        {
            char* err = dlerror();
            printf("[TheTest] libdl churn dlclose failed iter=%u: %s\n",
                   (unsigned int) i,
                   err ? err : "unknown");
            ok = false;
            break;
        }
    }

    if (dlclose(handle_local) < 0)
    {
        char* err = dlerror();
        printf("[TheTest] libdl first dlclose failed: %s\n", err ? err : "unknown");
        ok = false;
    }
    if (dlclose(handle_global) < 0)
    {
        char* err = dlerror();
        printf("[TheTest] libdl second dlclose failed: %s\n", err ? err : "unknown");
        ok = false;
    }

    if (dlclose(handle_global) == 0)
    {
        printf("[TheTest] libdl invalid-handle dlclose unexpectedly succeeded\n");
        ok = false;
    }
    else
    {
        char* err = dlerror();
        if (!err)
        {
            printf("[TheTest] libdl invalid-handle dlclose did not set an error\n");
            ok = false;
        }
    }

    if (ok)
        printf("[TheTest] libdl probe: OK\n");
    else
        printf("[TheTest] libdl probe: FAILED\n");
}

int main(int argc, char** argv, char** envp)
{
    (void) argc;
    (void) argv;
    (void) envp;

#ifdef TEST_DRM_KMS
    if (clear_screen() < 0)
        printf("[TheTest] clear_screen failed errno=%d\n", errno);
    thetest_drm_kms_probe();
#endif

#ifdef TEST_UNDEFINED_SYSCALL
    long rc = syscall(SYS_UNDEFINED_TEST, 0, 0, 0, 0, 0, 0);
    printf("[TheTest] undefined syscall(%ld) rc=%ld\n", SYS_UNDEFINED_TEST, rc);
#endif

#ifdef TEST_MMAP_FORBIDDEN
    printf("[TheTest] try mmap forbidden addr @ %p len=0x%llx\n",
           (void*) (uintptr_t) KERNEL_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    void* forbidden_map = mmap((void*) (uintptr_t) KERNEL_TEST_ADDR,
                               (size_t) MMAP_TEST_LEN,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS,
                               -1,
                               0);
    if (forbidden_map == MAP_FAILED)
        printf("[TheTest] mmap forbidden refused (expected)\n");
    else
    {
        printf("[TheTest] mmap forbidden succeeded (unexpected) ptr=%p\n",
               forbidden_map);
        int unmap_rc = munmap(forbidden_map, (size_t) MMAP_TEST_LEN);
        printf("[TheTest] cleanup unmap rc=%d\n", unmap_rc);
    }
#endif

#ifdef TEST_MMAP_LEN_EDGECASES
    thetest_mmap_probe("len=0", USER_MMAP_BASE, 0, true);
    thetest_mmap_probe("len=-1", USER_MMAP_BASE, (size_t) -1, true);
    thetest_mmap_probe("len=max-2048", USER_MMAP_BASE, ((size_t) -1) - 2048U, true);
    thetest_mmap_probe("len=window", USER_MMAP_BASE, (size_t) USER_MMAP_WINDOW_LEN, true);
    thetest_mmap_probe("len=window+1page", USER_MMAP_BASE, (size_t) (USER_MMAP_WINDOW_LEN + MMAP_TEST_LEN), true);
    thetest_mmap_probe("len=64MiB", USER_MMAP_BASE, (size_t) MMAP_LEN_64MIB, false);
    thetest_mmap_probe("forbidden+len=-1", KERNEL_TEST_ADDR, (size_t) -1, true);
#endif

#ifdef TEST_UNMAP_EDGECASES
    thetest_unmap_probe();
#endif

#ifdef TEST_RACE_CONCURRENCY
    thetest_race_probe();
#endif

#ifdef TEST_HEAP_STRESS
    thetest_heap_stress_probe();
#endif

#ifdef TEST_PREEMPTIVE_TIMER
    thetest_preemptive_probe();
#endif

#ifdef TEST_COW_FORK
    thetest_cow_probe();
#endif

#ifdef TEST_PTHREAD_SHIM
    thetest_pthread_probe();
#endif

#ifdef TEST_TLS_DYNAMIC
    thetest_tls_dynamic_probe();
#endif

#ifdef TEST_AUDIO_DSP
    thetest_audio_dsp_probe();
#endif

#ifdef TEST_LIBDL
    thetest_libdl_probe();
#endif

    volatile uint64_t* kernel_ptr = (volatile uint64_t*) (uintptr_t) KERNEL_TEST_ADDR;
#ifdef TEST_READ_KERNEL
    printf("[TheTest] try kernel read @ %p\n", (void*) (uintptr_t) KERNEL_TEST_ADDR);
    uint64_t value = *kernel_ptr;
    printf("[TheTest] kernel read succeeded (unexpected): 0x%llx\n", (unsigned long long) value);
#endif

#ifdef TEST_WRITE_KERNEL
    printf("[TheTest] try kernel write @ %p\n", (void*) (uintptr_t) KERNEL_TEST_ADDR);
    *kernel_ptr = 0xDEADBEEFDEADBEEFULL;
    printf("[TheTest] kernel write succeeded (unexpected)\n");
#endif

    return 0;
}
