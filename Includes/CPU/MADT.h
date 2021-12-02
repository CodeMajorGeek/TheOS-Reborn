#ifndef _MADT_H
#define _MADT_H

#include <stdint.h>
#include <string.h>

#define MADT_LAPIC_MAX_ENTRIES  256

uint8_t MADT_LAPIC_ids[MADT_LAPIC_MAX_ENTRIES] = { 0 }; // CPU core Local APIC IDs.
uint8_t MADT_num_core = 0;                              // Number of core detected.
uintptr_t MADT_LAPIC_ptr = 0;                           // Pointer of the Local APIC MMIO registers.
uintptr_t MADT_IOAPIC_ptr = 0;                          // Pointer to the IO APIC MMIO registers.

void MADT_detect_cores(uint8_t* rsdt);

#endif