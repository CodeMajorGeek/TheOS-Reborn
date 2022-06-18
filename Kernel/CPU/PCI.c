#include <CPU/PCI.h>

#include <CPU/IO.h>
#include <Device/AHCI.h>
#include <Memory/VMM.h>
#include <Memory/PMM.h>
#include <Storage/SATA.h>

#include <stdio.h>

PCI_func_t PCI_AHCI = { .vendor_id = AHCI_PCI_VENDOR_ID,
	                    .device_id = AHCI_PCI_DEVICE_ID,
	                    .MMIO_reg  = AHCI_PCI_MMIO_REG,
	                    .interrupt_reg = AHCI_PCI_INT_REG };

void PCI_init(void)
{
    if (PCI_scan_bus(&PCI_AHCI))
    {
        printf("PCI bus for AHCI controller found !\n");

        PCI_get_MMIO_space_size(&PCI_AHCI);

        printf("\tMMIO space is %d !\n", PCI_AHCI.MMIO_reg_size);
	    printf("\tMMIO address is 0x%X !\n", PCI_AHCI.MMIO_reg_addr);
    } else
		printf("Cannot find PCI bus for AHCI controller !\n");
}

uint16_t PCI_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address;
    uint32_t lbus  = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) func;
    uint16_t tmp = 0;
 
    // Create configuration address as per Figure 1
    address = (uint32_t) ((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t) 0x80000000));
 
    // Write out the address
    IO_outl(0xCF8, address);
    // Read in the data
    // (offset & 2) * 8) = 0 will choose the first word of the 32-bit register
    tmp = (uint16_t) ((IO_inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
    return tmp;
}

uint32_t PCI_read_word(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset)
{
    uint32_t lbus = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) func;
    uint32_t tmp = 0;

    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8)
            | (offset & 0xfc) | ((uint32_t) 0x80000000));

    IO_outl(0xCF8, address);

    return (uint32_t) IO_inl(0xCFC);
}

void PCI_get_MMIO_space_size(PCI_func_t* PCI_device)
{
    uint32_t lbus = PCI_device->bus_num;
    uint32_t lslot = PCI_device->slot_num;
    uint32_t offset = PCI_device->MMIO_reg;
    uint32_t lfunc = 0;
    
    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t) 0x80000000));

    IO_outl(0xCF8, address);
    IO_outl(0xCFC, 0xFFFFFFFF);

    uint32_t tmp = PCI_read_word(PCI_device->bus_num, PCI_device->slot_num, 0, PCI_device->MMIO_reg);
    PCI_device->MMIO_reg_size = ~tmp + 1;

    IO_outl(0xCF8, address);
    IO_outl(0xCFC, PCI_device->MMIO_reg_addr);
}

bool PCI_scan_bus(PCI_func_t* PCI_device)
{
    uint16_t vendor;
    uint16_t device;

    for (uint8_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t slot = 0; slot < 32; slot++)
        {
            vendor = PCI_config_read_word(bus, slot, 0, 0);
            device = PCI_config_read_word(bus, slot, 0, 0x02);

            if (vendor == PCI_device->vendor_id && device == PCI_device->device_id)
            {
                PCI_device->bus_num = bus;
                PCI_device->slot_num = slot;
                PCI_device->MMIO_reg_addr = PCI_read_word(bus, slot, 0, PCI_device->MMIO_reg);
                PCI_device->IRQ_line = PCI_read_word(bus, slot, 0, PCI_device->interrupt_reg) & 0xFF;

                return TRUE;
            }
        }
    }

    return FALSE;
}

void PCI_change_IRQ(PCI_func_t* PCI_device, int IRQ)
{
    uint32_t lbus = PCI_device->bus_num;
    uint32_t lslot = PCI_device->slot_num;
    uint32_t lfunc = 0;

    uint16_t offset = (uint16_t) PCI_device->interrupt_reg;
    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t) 0x80000000));

    uint32_t val = PCI_read_word(lbus, lslot, 0, PCI_device->interrupt_reg);

    IO_outl(0xCF8, address);
    IO_outl(0xCFC, (val & 0xFFFFFF00) | IRQ);

    PCI_device->IRQ_line = PCI_read_word(lbus, lslot, 0, PCI_device->interrupt_reg & 0xFF);
}