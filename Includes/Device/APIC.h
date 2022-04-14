#ifndef _APIC_H
#define _APIC_H

#include <CPU/ACPI.h>

#include <stdint.h>
#include <stdbool.h>

#define APIC_PROCESSOR_LOCAL1_TYPE          0
#define APIC_IO_TYPE                        1
#define APIC_IO_INT_SOURCE_OVERRIDE_TYPE    2
#define APIC_IO_NMASK_INT_SOURCE_TYPE       3
#define APIC_LOCAL_NMASK_INT_TYPE           4
#define APIC_LOCAL_ADDR_OVERRIDE_TYPE       5
#define APIC_PROCESSOR_LOCAL2_TYPE          9


typedef struct APIC_MADT_record
{
    uint8_t type;
    uint8_t length;
} APIC_MADT_record_t;

typedef struct APIC_proc_local1
{
    uint8_t ACPI_proc_ID;
    uint8_t APIC_ID;
    uint32_t flags;
} APIC_proc_local1_t;

typedef struct APIC_IO
{
    uint8_t IO_APIC_ID;
    uint8_t reserved;
    uint32_t IO_APIC_address;
    uint32_t GSI_base;
} APIC_IO_t;

typedef struct APIC_IO_int_source_override
{
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t GSI;
    uint16_t flags;
} APIC_IO_int_source_override_t;

typedef struct APIC_IO_nmask_int_source
{
    uint8_t NMI_source;
    uint8_t reserved;
    uint16_t flags;
    uint32_t GSI;
} APIC_IO_nmask_int_source_t;

typedef struct APIC_local_nmask_int
{
    uint8_t ACPI_proc_ID;
    uint16_t flags;
    uint8_t LINT;
} APIC_local_nmask_int_t;

typedef struct APIC_local_addr_override
{
    uint16_t reserved;
    uint64_t phys_addr;
} APIC_local_addr_override_t;

typedef struct APIC_proc_local2
{
    uint16_t reserved;
    uint32_t APIC_proc_local_ID;
    uint32_t flags;
    uint32_t ACPI_ID;

} APIC_proc_local2_t;

typedef struct APIC_MADT
{
    ACPI_SDT_header_t SDT_header;
    uint32_t lapic_ptr;
    uint32_t flags;
    APIC_MADT_record_t records[];
} APIC_MADT_t;

bool APIC_check(void);

void APIC_set_base(uintptr_t apic);
uintptr_t APIC_get_base(void);

void APIC_enable(void);

void APIC_detect_cores(APIC_MADT_t* madt);

void APIC_init(void);

#endif