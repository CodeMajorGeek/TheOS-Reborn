#include <Debug/Spinlock.h>

#include <CPU/x86.h>
#include <Memory/PMM.h>

static inline void spin_pause(void)
{
    __asm__ __volatile__("pause");
}

static inline uint64_t spin_read_rflags(void)
{
    uint64_t flags = 0;
    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(flags));
    return flags;
}

void spinlock_init(spinlock_t* lock)
{
    if (!lock)
        return;

    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELAXED);
}

bool spin_try_lock(spinlock_t* lock)
{
    if (!lock)
        return false;

    return !__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE);
}

void spin_lock(spinlock_t* lock)
{
    if (!lock)
        return;

    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
            spin_pause();
    }
}

void spin_unlock(spinlock_t* lock)
{
    if (!lock)
        return;

    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t* lock)
{
    uint64_t flags = spin_read_rflags();
    cli();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags)
{
    spin_unlock(lock);

    if (flags & (1ULL << 9))
        sti();
}

void get_caller_pcs(void* v, uintptr_t pcs[])
{
    uintptr_t* ebp;
    __asm__ __volatile__("mov %%rbp, %0" : "=r" (ebp));
    get_stack_pcs(ebp, pcs);
}

void get_stack_pcs(uintptr_t* ebp, uintptr_t pcs[])
{
    int i = 0;
    for (; i < 10; i++)
    {
        if (ebp == 0 || ebp < (uintptr_t*) PMM_get_kernel_start() || ebp == (uintptr_t*) 0xFFFFFFFF)
            break;

        pcs[i] = ebp[1];
        ebp = (uintptr_t*) ebp[0];
    }

    for (; i < 10; i++)
        pcs[i] = 0;
}
