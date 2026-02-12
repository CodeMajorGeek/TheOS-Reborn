#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct spinlock
{
    volatile uint32_t locked;
} spinlock_t;

void spinlock_init(spinlock_t* lock);
bool spin_try_lock(spinlock_t* lock);
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

uint64_t spin_lock_irqsave(spinlock_t* lock);
void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags);

void get_caller_pcs(void* v, uintptr_t pcs[]);
void get_stack_pcs(uintptr_t* ebp, uintptr_t pcs[]);

#endif
