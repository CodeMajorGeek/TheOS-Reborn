#ifndef _PCI_H
#define _PCI_H

#include <CPU/IO.h>
#include <Device/AHCI.h>

#include <stdint.h>
#include <stdbool.h>

#define PCI_AHCI_VENDOR_ID  0x8086
#define PCI_AHCI_DEVICE_ID  0x2922
#define PCI_AHCI_MMIO_REG   0x24
#define PCI_AHCI_INT_REG    0x3C

typedef struct PCI_func
{
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t bus_num;
    uint32_t slot_num;
    uint32_t MMIO_reg;
    uint32_t interrupt_reg;
    uint32_t MMIO_reg_addr;
    uint32_t MMIO_reg_size;
    uint32_t IRQ_line;
    uint32_t start_virtual_address;
} PCI_func_t;

void PCI_init(void);

uint16_t PCI_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t PCI_read_word(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset);

void PCI_get_MMIO_space_size(PCI_func_t* PCI_device);

bool PCI_scan_bus(PCI_func_t* PCI_device);

void PCI_change_IRQ(PCI_func_t* PCI_device, int IRQ);

#endif