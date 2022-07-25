#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include <stdint.h>

void get_caller_pcs(void* v, uintptr_t pcs[]);
void get_stack_pcs(uintptr_t* ebp, uintptr_t pcs[]);

#endif