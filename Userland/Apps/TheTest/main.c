#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

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
// #define TEST_READ_KERNEL
// #define TEST_WRITE_KERNEL

static void thetest_mmap_probe(const char* label, uintptr_t addr, size_t len, bool expect_fail)
{
    printf("[TheTest] mmap test '%s': addr=%p len=0x%llx\n",
           label,
           (void*) addr,
           (unsigned long long) len);

    void* map_ptr = sys_map((void*) addr, len, SYS_PROT_READ | SYS_PROT_WRITE);
    if (!map_ptr)
    {
        printf("[TheTest] mmap %s\n", expect_fail ? "refused (expected)" : "failed (unexpected)");
        return;
    }

    printf("[TheTest] mmap %s ptr=%p\n",
           expect_fail ? "succeeded (unexpected)" : "succeeded",
           map_ptr);

    int unmap_rc = sys_unmap(map_ptr, len);
    printf("[TheTest] unmap rc=%d\n", unmap_rc);
}

static void thetest_unmap_probe(void)
{
    printf("[TheTest] unmap test 'unmapped zone': addr=%p len=0x%llx\n",
           (void*) (uintptr_t) UNMAP_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    int unmapped_rc = sys_unmap((void*) (uintptr_t) UNMAP_TEST_ADDR, (size_t) MMAP_TEST_LEN);
    printf("[TheTest] unmap on unmapped zone rc=%d (expected<0)\n", unmapped_rc);

    printf("[TheTest] unmap test 'double unmap': map addr=%p len=0x%llx\n",
           (void*) (uintptr_t) UNMAP_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    void* map_ptr = sys_map((void*) (uintptr_t) UNMAP_TEST_ADDR,
                            (size_t) MMAP_TEST_LEN,
                            SYS_PROT_READ | SYS_PROT_WRITE);
    if (!map_ptr)
    {
        printf("[TheTest] setup map failed (unexpected)\n");
        return;
    }

    int first_unmap_rc = sys_unmap(map_ptr, (size_t) MMAP_TEST_LEN);
    int second_unmap_rc = sys_unmap(map_ptr, (size_t) MMAP_TEST_LEN);
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

    uint8_t raw[64];
    size_t out_size = 0;
    if (fs_read(path, raw, sizeof(raw) - 1U, &out_size) != 0)
        return false;

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

    int fd = sys_open(path, SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_TRUNC);
    if (fd < 0)
        return false;

    int write_rc = fs_write(fd, text, (size_t) text_len);
    int close_rc = sys_close(fd);
    return write_rc == text_len && close_rc == 0;
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
        int status = 0;
        int signal = 0;
        int rc = sys_waitpid(pid, &status, &signal);
        if (rc == pid)
        {
            if (out_status)
                *out_status = status;
            if (out_signal)
                *out_signal = signal;
            return rc;
        }

        if (rc < 0)
            return rc;

        (void) sys_sleep_ms(1);
        waited_ms++;
        if (timeout_ms != 0U && waited_ms >= timeout_ms)
            return -2;
    }
}

static void thetest_race_worker(const char* path, int worker_id)
{
    for (int i = 0; i < RACE_ITERS; i++)
    {
        uint64_t current = 0;
        if (!thetest_read_counter(path, &current))
        {
            printf("[TheTest][worker:%d] read failed iter=%d\n", worker_id, i);
            sys_exit(2);
        }

        if ((i & 1) == 0)
            (void) sys_yield();

        uint64_t next = current + 1ULL;
        if (!thetest_write_counter(path, next))
        {
            printf("[TheTest][worker:%d] write failed iter=%d\n", worker_id, i);
            sys_exit(3);
        }

        if ((i & 3) == 0)
            (void) sys_yield();
    }

    sys_exit(0);
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
        int pid = sys_fork();
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

int main(void)
{
#ifdef TEST_UNDEFINED_SYSCALL
    long rc = syscall(SYS_UNDEFINED_TEST, 0, 0, 0, 0, 0, 0);
    printf("[TheTest] undefined syscall(%ld) rc=%ld\n", SYS_UNDEFINED_TEST, rc);
#endif

#ifdef TEST_MMAP_FORBIDDEN
    printf("[TheTest] try mmap forbidden addr @ %p len=0x%llx\n",
           (void*) (uintptr_t) KERNEL_TEST_ADDR,
           (unsigned long long) MMAP_TEST_LEN);
    void* forbidden_map = sys_map((void*) (uintptr_t) KERNEL_TEST_ADDR,
                                  (size_t) MMAP_TEST_LEN,
                                  SYS_PROT_READ | SYS_PROT_WRITE);
    if (!forbidden_map)
        printf("[TheTest] mmap forbidden refused (expected)\n");
    else
    {
        printf("[TheTest] mmap forbidden succeeded (unexpected) ptr=%p\n",
               forbidden_map);
        int unmap_rc = sys_unmap(forbidden_map, (size_t) MMAP_TEST_LEN);
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
