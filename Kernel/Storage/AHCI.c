#include <Storage/AHCI.h>

#include <CPU/PCI.h>
#include <CPU/IO.h>

#include <stdbool.h>

static AHCI_device_t AHCI_devices[] =
{
    { AHCI_VENDOR_INTEL, AHCI_ICH7_SATA, "Intel ICH7 SATA Controller" },
    { AHCI_VENDOR_INTEL, 0x2829, "Intel ICH8M" },
    { AHCI_VENDOR_INTEL, 0x1922, "Intel ICH9" },
    { AHCI_VENDOR_INTEL, 0x2922, "Intel ICH9R" },
    { AHCI_VENDOR_INTEL, 0x1E03, "Intel Panther Point" },
    { AHCI_VENDOR_VMWARE, 0x07E0, "VMWare SATA" },
    { AHCI_VENDOR_VMWARE, 0x2829, "VMWare PCIE Root" },
    { 0, 0, "" } // Terminal node.
};

void AHCI_try_setup_device(uint16_t bus, uint32_t slot, uint16_t func)
{
    /*
    uint16_t vendor = AHCI_probe(bus, slot, func, AHCI_VENDOR_OFFSET);
    uint16_t device = AHCI_probe(bus, slot, func, AHCI_DEVICE_OFFSET);
    
    printf("AHCI device vendor=0x%X, id=0x%X \n", vendor, device);

    AHCI_device_hacks(bus, slot, func, vendor, device);

    uintptr_t AHCI_base = (uintptr_t) AHCI_read(bus, slot, func, AHCI_ABAR5_OFFSET);
    printf("\t Current ABAR5 offset: 0x%X !\n", AHCI_base);

    if (AHCI_base != 0 && AHCI_base != 0xFFFFFFFF)
    {
        const char* name;
        bool identified = false;

        for (uint16_t i = 0; AHCI_devices[i].vendor != 0; i++)
        {
            if (AHCI_devices[i].vendor == vendor && AHCI_devices[i].device == device) {
                name = AHCI_devices[i].name;
                identified = true;
            }

            if (identified)
            {
                printf("AHCI found: %s !\n", name);
                AHCI_try_setup_known_device((char*) name, AHCI_base, bus, slot, func);
            }
                
        }
    }
    */
}

void AHCI_try_setup_known_device(char* name, uintptr_t AHCI_base, uint16_t bus, uint16_t slot, uint16_t func)
{

}

void AHCI_device_hacks(uint16_t bus, uint16_t slot, uint16_t func, uint16_t vendor, uint16_t device)
{
    if (vendor == AHCI_VENDOR_INTEL && device == AHCI_ICH7_SATA)
        AHCI_writeb(bus, slot, func, 0x90, 0x40); // Enable AHCI mode.
}

void AHCI_writeb(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset, uint8_t data) // FIX: Where the data must be redirected to ?
{
    uint32_t address = AHCI_get_address(bus, slot, func, offset);
    IO_outl(AHCI_HBA_PORT, address);
}

uint64_t AHCI_read(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset)
{
    uint32_t address = AHCI_get_address(bus, slot, func, offset);
    IO_outl(AHCI_HBA_PORT, address);

    return (uint64_t) IO_inl(0xCFC);
}