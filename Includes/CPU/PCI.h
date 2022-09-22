#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include <stdio.h>

#define PCI_CONF_ADDR_IO_PORT   0xCF8
#define PCI_CONF_DATA_IO_PORT   0xCFC

#define PCI_DEV_CLASS_STORAGE   0x01
#define PCI_DEV_CLASS_BRIDGE    0x06

#define PCI_VENDOR_REG          0x00
#define PCI_DEVICE_REG          0x02
#define PCI_BIST_REG            0x0C
#define PCI_CLASS_REG           0x08
#define PCI_SECONDARY_BUS_REG   0x1A
#define PCI_BAR5_ADDR_REG       0x24

typedef struct PCI_bus PCI_bus_t;

void PCI_init(void);

uint32_t PCI_config_read(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t PCI_config_readw(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);

void PCI_check_device(uint8_t bus, uint8_t slot);
void PCI_check_function(uint8_t bus, uint8_t slot, uint8_t function);

void PCI_scan_bus(uint8_t bus);

static void PCI_try_attach(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_storage_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);

#endif