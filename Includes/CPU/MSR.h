#ifndef _MSR_H
#define _MSR_H

#include <cpuid.h>
#include <stdint.h>

void MSR_get(uint32_t msr, uint32_t* lo, uint32_t* hi);
void MSR_set(uint32_t msr, uint32_t lo, uint32_t hi);

#endif