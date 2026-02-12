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

#define APIC_IMCR_CTRL_REG                  0x22
#define APIC_IMCR_DATA_REG                  0x23

#define APIC_IMCR_ACCESS                    0x70
#define APIC_FORCE_NMI_INTR_SIG             0x01

#define APIC_APICID                         0x20
#define APIC_APICVER	                    0x30
#define APIC_TASKPRIOR	                    0x80
#define APIC_EOI	                        0x0B0
#define APIC_LDR	                        0x0D0
#define APIC_DFR	                        0x0E0
#define APIC_SPURIOUS	                    0x0F0
#define APIC_ESR	                        0x280
#define APIC_ICRL	                        0x300
#define APIC_ICRH	                        0x310
#define APIC_LVT_TMR	                    0x320
#define APIC_LVT_PERF	                    0x340
#define APIC_LVT_LINT0                  	0x350
#define APIC_LVT_LINT1	                    0x360
#define APIC_LVT_ERR	                    0x370
#define APIC_TMRINITCNT	                    0x380
#define APIC_TMRCURRCNT	                    0x390
#define APIC_TMRDIV	                        0x3E0
#define APIC_LAST	                        0x38F
#define APIC_DISABLE	                    0x10000
#define APIC_SW_ENABLE	                    0x100
#define APIC_CPUFOCUS	                    0x200
#define APIC_NMI	                        (4 << 8)
#define TMR_PERIODIC	                    0x20000
#define TMR_BASEDIV	                        (1 << 20)

// ACPI MADT interrupt source override INTI flags.
#define APIC_MADT_INTI_POLARITY_MASK            0x3U
#define APIC_MADT_INTI_POLARITY_CONFORMS        0x0U
#define APIC_MADT_INTI_POLARITY_ACTIVE_HIGH     0x1U
#define APIC_MADT_INTI_POLARITY_RESERVED        0x2U
#define APIC_MADT_INTI_POLARITY_ACTIVE_LOW      0x3U

#define APIC_MADT_INTI_TRIGGER_SHIFT            2U
#define APIC_MADT_INTI_TRIGGER_MASK             (0x3U << APIC_MADT_INTI_TRIGGER_SHIFT)
#define APIC_MADT_INTI_TRIGGER_CONFORMS         0x0U
#define APIC_MADT_INTI_TRIGGER_EDGE             0x1U
#define APIC_MADT_INTI_TRIGGER_RESERVED         0x2U
#define APIC_MADT_INTI_TRIGGER_LEVEL            0x3U

// IOAPIC redirection entry low dword bits.
#define APIC_IORED_POLARITY_LOW                 (1U << 13)
#define APIC_IORED_TRIGGER_LEVEL                (1U << 15)
#define APIC_IORED_MASK                         (1U << 16)

// LAPIC ICR helpers.
#define APIC_ICR_DELIVERY_STATUS                (1U << 12)
#define APIC_ICR_LEVEL_ASSERT                   (1U << 14)
#define APIC_ICR_TRIGGER_LEVEL                  (1U << 15)
#define APIC_ICR_DELIVERY_INIT                  (0x5U << 8)
#define APIC_ICR_DELIVERY_STARTUP               (0x6U << 8)


typedef struct APIC_MADT
{
    ACPI_SDT_header_t SDT_header;
    uint32_t lapic_ptr;
    uint32_t flags;
    uint8_t records[];
} __attribute__((__packed__)) APIC_MADT_t;

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

uint32_t APIC_local_read(uint32_t offset);
void APIC_local_write(uint32_t offset, uint32_t value);

uint32_t APIC_IO_read(uint8_t index, uint32_t reg);
void APIC_IO_write(uint8_t index, uint32_t reg, uint32_t value);

bool APIC_is_enabled(void);
uint8_t APIC_get_current_lapic_id(void);
uint8_t APIC_get_bsp_lapic_id(void);
uint8_t APIC_get_core_count(void);
uint8_t APIC_get_core_id(uint8_t index);
void APIC_register_IRQ_vector(int vec, int irq, bool disable);
void APIC_register_GSI_vector(int vec, uint32_t gsi, bool disable);
bool APIC_send_ipi(uint8_t apic_id, uint8_t vector);
bool APIC_startup_ap(uint8_t apic_id, uint8_t startup_vector);
void APIC_send_EOI(void);
bool APIC_timer_init_bsp(uint32_t hz);

#endif
