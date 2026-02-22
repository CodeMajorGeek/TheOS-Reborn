#ifndef _MSR_H
#define _MSR_H

#include <stdint.h>

#define IA32_APIC_BASE_MSR          0x1B
#define IA32_APIC_BASE_MSR_BSP      0x100   // Processor is a BSP.
#define IA32_APIC_BASE_MSR_X2APIC   0x400
#define IA32_APIC_BASE_MSR_ENABLE   0x800

#define IA32_EFER   0xC0000080
#define IA32_EFER_NXE           (1ULL << 11)
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084
#define IA32_FS_BASE            0xC0000100
#define IA32_GS_BASE            0xC0000101
#define IA32_KERNEL_GS_BASE     0xC0000102

uint64_t MSR_get(uint32_t msr);
void MSR_set(uint32_t msr, uint64_t value);

#endif
