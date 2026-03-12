#ifndef _AHCI_PRIVATE_H
#define _AHCI_PRIVATE_H

#include <Storage/AHCI.h>
#include <CPU/ISR.h>

static void AHCI_irq_handler(interrupt_frame_t* frame);
static void AHCI_setup_interrupts(uint16_t bus, uint32_t slot, uint16_t function);
static void AHCI_init_wait_queues(void);
static int AHCI_prepare_cmd(HBA_PORT_t* port, uint32_t byte_count, uint8_t* buf, AHCI_cmd_context_t* out_ctx);
static int AHCI_submit_cmd(HBA_PORT_t* port, const AHCI_cmd_context_t* cmd_ctx);
static bool AHCI_register_device(HBA_PORT_t* port, const char* kind, uint32_t* out_device_num);
static bool AHCI_identify_capacity_sectors(HBA_PORT_t* port, uint64_t* out_sector_count);
static bool AHCI_get_cached_capacity_sectors(HBA_PORT_t* port, uint64_t* out_sector_count);
static int AHCI_atapi_read_blocks(HBA_PORT_t* port, uint32_t lba, uint32_t count, uint8_t* buf);
static int AHCI_atapi_read_512(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint8_t* buf);
static void AHCI_ATAPI_init(HBA_PORT_t* port, int num);

#endif
