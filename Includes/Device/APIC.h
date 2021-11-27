#ifndef _APIC_H
#define _APIC_H

#include <cpuid.h>
#include <stdint.h>
#include <stdbool.h>

#define IA32_APIC_BASE_MSR          0x1B
#define IA32_APIC_BASE_MSR_BSP      0x100 // Processor is a BSP.
#define IA32_APIC_BASE_MSR_ENABLE   0x800

bool APIC_check(void);

#endif