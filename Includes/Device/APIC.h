#ifndef _APIC_H
#define _APIC_H

#include <stdint.h>
#include <stdbool.h>

#define IA32_APIC_BASE_MSR          0x1B
#define IA32_APIC_BASE_MSR_BSP      0x100 // Processor is a BSP.
#define IA32_APIC_BASE_MSR_ENABLE   0x800

bool APIC_check(void);

void APIC_set_base(uintptr_t apic);
uintptr_t APIC_get_base(void);

void APIC_enable(void);

#endif