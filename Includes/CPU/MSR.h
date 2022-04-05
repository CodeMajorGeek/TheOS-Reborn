#ifndef _MSR_H
#define _MSR_H

#include <stdint.h>

#define IA32_EFER   0xC0000080

void MSR_get(uint32_t msr, uint32_t* lo, uint32_t* hi);
void MSR_set(uint32_t msr, uint32_t lo, uint32_t hi);

#endif