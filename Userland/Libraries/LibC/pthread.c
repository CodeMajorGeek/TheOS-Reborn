#include <pthread.h>

#include <errno.h>
#include <libc_tls.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syscall.h>

/* Le rendu WindowServer + libc peut monter haut en pile ; 256 KiB a montré des #PF
 * avec cr2 cohérent avec rdi corrompu dans memcpy (thread de rendu). */
#define LIBC_PTHREAD_DEFAULT_STACK_SIZE (2U * 1024U * 1024U)
#define LIBC_PTHREAD_MAX_TRACKED        128U

typedef struct libc_pthread_start
{
    void* (*start_routine)(void*);
    void* arg;
    libc_tls_region_t tls_region;
} libc_pthread_start_t;

typedef struct libc_pthread_record
{
    int used;
    pthread_t tid;
    void* stack_base;
    size_t stack_size;
    libc_tls_region_t tls_region;
} libc_pthread_record_t;

static volatile int LibC_pthread_table_lock = 0;
static libc_pthread_record_t LibC_pthread_records[LIBC_PTHREAD_MAX_TRACKED];

static void LibC_pthread_lock(void)
{
    while (__atomic_test_and_set(&LibC_pthread_table_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_pthread_table_lock, __ATOMIC_RELAXED) != 0)
            (void) sched_yield();
    }
}

static void LibC_pthread_unlock(void)
{
    __atomic_clear(&LibC_pthread_table_lock, __ATOMIC_RELEASE);
}

static int LibC_pthread_track_add(pthread_t tid,
                                  void* stack_base,
                                  size_t stack_size,
                                  const libc_tls_region_t* tls_region)
{
    int slot = -1;
    LibC_pthread_lock();
    for (size_t i = 0; i < LIBC_PTHREAD_MAX_TRACKED; i++)
    {
        if (!LibC_pthread_records[i].used)
        {
            slot = (int) i;
            LibC_pthread_records[i].used = 1;
            LibC_pthread_records[i].tid = tid;
            LibC_pthread_records[i].stack_base = stack_base;
            LibC_pthread_records[i].stack_size = stack_size;
            if (tls_region)
                LibC_pthread_records[i].tls_region = *tls_region;
            else
                memset(&LibC_pthread_records[i].tls_region, 0, sizeof(LibC_pthread_records[i].tls_region));
            break;
        }
    }
    LibC_pthread_unlock();
    return slot >= 0;
}

static int LibC_pthread_track_take(pthread_t tid,
                                   void** stack_base,
                                   size_t* stack_size,
                                   libc_tls_region_t* tls_region)
{
    int found = 0;
    LibC_pthread_lock();
    for (size_t i = 0; i < LIBC_PTHREAD_MAX_TRACKED; i++)
    {
        if (!LibC_pthread_records[i].used || LibC_pthread_records[i].tid != tid)
            continue;

        if (stack_base)
            *stack_base = LibC_pthread_records[i].stack_base;
        if (stack_size)
            *stack_size = LibC_pthread_records[i].stack_size;
        if (tls_region)
            *tls_region = LibC_pthread_records[i].tls_region;
        LibC_pthread_records[i].used = 0;
        LibC_pthread_records[i].tid = 0;
        LibC_pthread_records[i].stack_base = NULL;
        LibC_pthread_records[i].stack_size = 0;
        memset(&LibC_pthread_records[i].tls_region, 0, sizeof(LibC_pthread_records[i].tls_region));
        found = 1;
        break;
    }
    LibC_pthread_unlock();
    return found;
}

static __attribute__((__noreturn__)) void LibC_pthread_entry(void* opaque)
{
    libc_pthread_start_t* start = (libc_pthread_start_t*) opaque;
    void* (*fn)(void*) = NULL;
    void* arg = NULL;
    libc_tls_region_t tls_region;
    memset(&tls_region, 0, sizeof(tls_region));
    if (start)
    {
        fn = start->start_routine;
        arg = start->arg;
        tls_region = start->tls_region;
        free(start);
    }

    if (__libc_tls_activate(tls_region.thread_pointer) < 0)
        sys_thread_exit((uint64_t) (uintptr_t) NULL);

    void* retval = NULL;
    if (fn)
        retval = fn(arg);

    pthread_exit(retval);
    __builtin_unreachable();
}

int pthread_create(pthread_t* thread,
                   const pthread_attr_t* attr,
                   void* (*start_routine)(void*),
                   void* arg)
{
    (void) attr;

    if (!thread || !start_routine)
        return EINVAL;

    size_t stack_size = LIBC_PTHREAD_DEFAULT_STACK_SIZE;
    void* stack_base = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_base == MAP_FAILED)
        return EAGAIN;

    libc_pthread_start_t* start = (libc_pthread_start_t*) malloc(sizeof(*start));
    if (!start)
    {
        (void) munmap(stack_base, stack_size);
        return EAGAIN;
    }

    start->start_routine = start_routine;
    start->arg = arg;
    if (__libc_tls_create(&start->tls_region) < 0)
    {
        free(start);
        (void) munmap(stack_base, stack_size);
        return EAGAIN;
    }
    libc_tls_region_t tls_region = start->tls_region;

    uintptr_t stack_top = ((uintptr_t) stack_base + stack_size) & ~(uintptr_t) 0xFULL;
    int tid = sys_thread_create_ex((uintptr_t) &LibC_pthread_entry,
                                   (uintptr_t) start,
                                   stack_top,
                                   tls_region.thread_pointer);
    if (tid <= 0)
    {
        __libc_tls_destroy(&tls_region);
        free(start);
        (void) munmap(stack_base, stack_size);
        return EAGAIN;
    }

    if (!LibC_pthread_track_add((pthread_t) tid, stack_base, stack_size, &tls_region))
    {
        uint64_t ignored = 0;
        while (sys_thread_join(tid, &ignored) == 0)
            (void) sched_yield();
        __libc_tls_destroy(&tls_region);
        (void) munmap(stack_base, stack_size);
        return EAGAIN;
    }

    *thread = (pthread_t) tid;
    return 0;
}

int pthread_join(pthread_t thread, void** retval)
{
    if (thread <= 0)
        return EINVAL;

    uint64_t raw_retval = 0;
    for (;;)
    {
        int rc = sys_thread_join((int) thread, &raw_retval);
        if (rc < 0)
            return ECHILD;

        if (rc == thread)
            break;

        (void) sched_yield();
    }

    if (retval)
        *retval = (void*) (uintptr_t) raw_retval;

    void* stack_base = NULL;
    size_t stack_size = 0;
    libc_tls_region_t tls_region;
    memset(&tls_region, 0, sizeof(tls_region));
    if (LibC_pthread_track_take(thread, &stack_base, &stack_size, &tls_region) &&
        stack_base && stack_size != 0)
    {
        (void) munmap(stack_base, stack_size);
    }
    __libc_tls_destroy(&tls_region);

    return 0;
}

void pthread_exit(void* retval)
{
    sys_thread_exit((uint64_t) (uintptr_t) retval);
}

pthread_t pthread_self(void)
{
    int tid = sys_thread_self();
    if (tid < 0)
        return 0;
    return (pthread_t) tid;
}

int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr)
{
    (void) attr;
    if (!mutex)
        return EINVAL;

    __atomic_store_n(&mutex->locked, 0, __ATOMIC_RELAXED);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    if (!mutex)
        return EINVAL;

    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex)
{
    if (!mutex)
        return EINVAL;

    while (__atomic_test_and_set(&mutex->locked, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&mutex->locked, __ATOMIC_RELAXED) != 0)
            (void) sched_yield();
    }

    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    if (!mutex)
        return EINVAL;

    __atomic_clear(&mutex->locked, __ATOMIC_RELEASE);
    return 0;
}
