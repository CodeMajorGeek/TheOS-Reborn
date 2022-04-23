#ifndef _APIC_H
#define _APIC_H

#include <CPU/ACPI.h>
#include <CPU/MSR.h>

#include <stdint.h>
#include <stdbool.h>

#define APIC_PROCESSOR_LOCAL1_TYPE          0
#define APIC_IO_TYPE                        1
#define APIC_IO_INT_SOURCE_OVERRIDE_TYPE    2
#define APIC_IO_NMASK_INT_SOURCE_TYPE       3
#define APIC_LOCAL_NMASK_INT_TYPE           4
#define APIC_LOCAL_ADDR_OVERRIDE_TYPE       5
#define APIC_PROCESSOR_LOCAL2_TYPE          9

#define APIC_PROCESSOR_ENABLED              1
#define APIC_ONLINE_CAPABLE                 (1 << 1)

#define APIC_LOCAL_ID_REG                   0x20
#define APIC_SPURIOUS_VEC_REG               0xF0
#define APIC_INT_CMD_REG_LO                 0x300
#define APIC_INT_CMD_REG_HI                 0x310

#define APIC_IMCR_CTRL_REG                  0x22
#define APIC_IMCR_DATA_REG                  0x23

#define APIC_IMCR_ACCESS                    0x70
#define APIC_FORCE_NMI_INTR_SIG             0x01

typedef struct APIC_MADT
{
    ACPI_SDT_header_t SDT_header;
    uint32_t lapic_ptr;
    uint32_t flags;
    uint8_t records[];
} APIC_MADT_t;

typedef struct APIC_IO
{
    uint8_t id;
    uint32_t ptr;
    uint32_t irq_base;
    uint32_t irq_end;
} APIC_IO_t;

bool APIC_check(void);

void APIC_enable(void);
void APIC_detect_cores(APIC_MADT_t* madt);
void APIC_disable_PIC_mode(void);
void APIC_init(APIC_MADT_t* madt);

uintptr_t APIC_get_base(void);
void APIC_set_base(uintptr_t apic);

uint64_t APIC_local_read(uint64_t offset);
void APIC_local_write(uint64_t offset, uint64_t value);

uint32_t APIC_IO_read(uint8_t index, uint32_t reg);
void APIC_IO_write(uint8_t index, uint32_t reg, uint32_t value);

#endif