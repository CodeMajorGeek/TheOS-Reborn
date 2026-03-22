#ifndef _E1000_PRIVATE_H
#define _E1000_PRIVATE_H

#include <Device/E1000.h>
#include <CPU/ISR.h>

static inline uint32_t E1000_mmio_read32(uint32_t reg);
static inline void E1000_mmio_write32(uint32_t reg, uint32_t value);
static inline uint16_t E1000_ring_next(uint16_t index, uint16_t count);

static bool E1000_is_supported_device(uint16_t vendor, uint16_t device);
static bool E1000_wait_ctrl_reset_clear(uint32_t timeout_loops);
static void E1000_read_mac_locked(void);
static bool E1000_alloc_rings_locked(void);
static void E1000_program_hw_rings_locked(void);
static void E1000_configure_rx_tx_locked(void);
static bool E1000_setup_interrupts_locked(uint8_t bus, uint8_t slot, uint8_t function);
static uint32_t E1000_poll_rx_locked(void);
static bool E1000_wait_rx_queue_empty_predicate(void* context);
static void E1000_irq_handler(interrupt_frame_t* frame);
static bool E1000_controller_init_locked(uint8_t bus,
                                         uint8_t slot,
                                         uint8_t function,
                                         uint16_t vendor,
                                         uint16_t device);

#endif
