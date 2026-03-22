#ifndef _ISR_H
#define _ISR_H

#include <CPU/IDT.h>

#define MAX_KNOWN_EXCEPTIONS    19

#define ISR_COUNT_BEFORE_IRQ    32      
#define MAX_IRQ_ENTRIES         16
#define IRQ_VECTOR_BASE         ISR_COUNT_BEFORE_IRQ
#define IRQ_VECTOR_END          (ISR_COUNT_BEFORE_IRQ + MAX_IRQ_ENTRIES - 1)
#define TICK_VECTOR             IRQ_VECTOR_BASE

#define TICK_LOG_PERIOD         50000ULL
#define PIC_CHECK_PERIOD        100ULL
#define PIC_QUIET_REPORT_AFTER  500ULL

#define IRQ0    32
#define IRQ1    33
#define IRQ2    34
#define IRQ3    35
#define IRQ4    36
#define IRQ5    37
#define IRQ6    38
#define IRQ7    39
#define IRQ8    40
#define IRQ9    41
#define IRQ10   42
#define IRQ11   43
#define IRQ12   44
#define IRQ13   45
#define IRQ14   46
#define IRQ15   47

typedef void (*IRQ_t)(interrupt_frame_t*);

typedef enum tick_source
{
    TICK_SOURCE_PIT_IOAPIC = 0,
    TICK_SOURCE_LAPIC_TIMER = 1
} tick_source_t;

typedef struct ISR_runtime_state
{
    IRQ_t irq_handlers[MAX_IRQ_ENTRIES];
    IRQ_t vector_handlers[IDT_MAX_VECTORS];
    uint8_t pic_irq_seen_while_apic;
    uint8_t pic_quiet_reported;
    uint8_t ioapic_vector_seen[MAX_IRQ_ENTRIES];
    uint64_t irq_vector_count[MAX_IRQ_ENTRIES];
    uint64_t timer_ticks;
    uint64_t tick_cpu_hits[256];
    uint64_t pic_activity_count;
    uint64_t irq_range_seen;
    tick_source_t tick_source;
    uint32_t tick_hz;
} ISR_runtime_state_t;

void ISR_register_IRQ(int index, IRQ_t irq);
void ISR_register_vector(uint8_t vector, IRQ_t handler);
void ISR_set_tick_source(tick_source_t source, uint32_t hz);
tick_source_t ISR_get_tick_source(void);
uint32_t ISR_get_tick_hz(void);

uint64_t ISR_get_timer_ticks(void);
uint64_t ISR_get_vector_count(uint8_t vector);
uint64_t ISR_get_pic_activity_count(void);

void ISR_handler(interrupt_frame_t* frame);
void IRQ_handler(interrupt_frame_t* frame);

#endif
