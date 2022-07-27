#include <CPU/PCI.h>

#include <Debug/Assert.h>
#include <Storage/AHCI.h>
#include <CPU/IO.h>

#include <string.h>

void PCI_init(void)
{
    printf("Probing PCI bus 0 !\n");

    PCI_scan_bus(0);
}

uint16_t PCI_config_readw(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    assert(slot < 32);
    assert(func < 8);

    uint32_t lbus = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) func;

    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | (1 << 31));

    IO_outl(PCI_CONF_ADDR_IO_PORT, address);
    return (uint16_t) ((IO_inl(PCI_CONF_DATA_IO_PORT) >> ((offset & 2) * 8)) & 0xFFFF);
}

void PCI_check_device(uint8_t bus, uint8_t slot)
{
    uint8_t function = 0;

    uint16_t vendor_id = PCI_config_readw(bus, slot, function, PCI_VENDOR_REG);
    if (vendor_id == 0xFFFF)
        return; // No device attached.

    uint16_t device_id = PCI_config_readw(bus, slot, function, PCI_DEVICE_REG);

    PCI_try_attach(bus, slot, function, vendor_id, device_id);
    
    PCI_check_function(bus, slot, function);
    uint8_t header_type = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_BIST_REG) & 0xFF);
    if ((header_type & 0x80) != 0) // It's a multi function device.
    {
        for (function = 1; function < 8; function++)
            PCI_check_function(bus, slot, function);
    }
}

void PCI_check_function(uint8_t bus, uint8_t slot, uint8_t function)
{
    uint8_t base_class = (uint8_t) ((PCI_config_readw(bus, slot, function, PCI_CLASS_REG) >> 8) & 0XFF);
    uint8_t sub_class = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_CLASS_REG) & 0XFF);

    if (base_class == 0x6 && sub_class == 0x4)
    {
        uint8_t secondary_bus = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_SECONDARY_BUS_REG) & 0XFF);
        PCI_scan_bus(secondary_bus);
    }
}

void PCI_scan_bus(uint8_t bus)
{
    for (uint8_t device = 0; device < 32; device++)
        PCI_check_device(bus, device);
}

static void PCI_try_attach(uint8_t bus, uint16_t slot, uint16_t function, uint16_t vendor, uint16_t device)
{
    uint8_t base_class = (uint8_t) ((PCI_config_readw(bus, slot, function, PCI_CLASS_REG) >> 8) & 0XFF); // TODO: Maybe try to execute once...
    switch (base_class)
    {
    case PCI_DEV_CLASS_STORAGE:
    case PCI_DEV_CLASS_BRIDGE:
        PCI_attach_storage_dev(bus, device, function, vendor, device);
        break;
    default:
        break;
    }
}

static void PCI_attach_storage_dev(uint8_t bus, uint16_t slot, uint16_t function, uint16_t vendor, uint16_t device)
{
    printf("Found PCI Mass storage on bus=%d, slot=%d, vendor=0x%X, device=0x%X !\n", bus, slot, vendor, device);
}