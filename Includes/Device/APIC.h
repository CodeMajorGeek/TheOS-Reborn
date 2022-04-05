#ifndef _APIC_H
#define _APIC_H

#include <stdint.h>
#include <stdbool.h>

bool APIC_check(void);

void APIC_set_base(uintptr_t apic);
uintptr_t APIC_get_base(void);

void APIC_enable(void);

#endif