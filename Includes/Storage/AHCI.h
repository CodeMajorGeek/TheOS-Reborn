#ifndef _AHCI_H
#define _AHCI_H

#include <stdint.h>

#define AHCI_HBA_PORT       0xCF8

#define AHCI_VENDOR_OFFSET  0x0
#define AHCI_DEVICE_OFFSET  0x02
#define AHCI_ABAR5_OFFSET   0x24

/* AHCI vendors: */
#define AHCI_VENDOR_INTEL   0x8086
#define AHCI_VENDOR_VMWARE  0x15AD

/* AHCI devices: */
#define AHCI_ICH7_SATA      0x27C0

typedef struct AHCI_device
{
    uint16_t vendor;
    uint16_t device;
    const char* name;
} AHCI_device_t;

static inline uint32_t AHCI_get_address(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset)
{
    uint32_t lbus = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) func;

    return (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t) 0x80000000));
}

uint16_t AHCI_probe(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset);
uint64_t AHCI_read(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset);
void AHCI_writeb(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset, uint8_t data);

void AHCI_try_setup_device(uint16_t bus, uint32_t slot, uint16_t func);
void AHCI_try_setup_known_device(char* name, uintptr_t AHCI_base, uint16_t bus, uint16_t slot, uint16_t func);

void AHCI_device_hacks(uint16_t bus, uint16_t slot, uint16_t func, uint16_t vendor, uint16_t device);


#endif