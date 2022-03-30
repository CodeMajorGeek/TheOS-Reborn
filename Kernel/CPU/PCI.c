#include <CPU/PCI.h>

static PCI_func_t PCI_AHCI;

static void* PCI_pages_for_ahci_start;
static void* PCI_pages_for_ahci_end;

void PCI_init(void)
{
    /* AHCI + SATA setup. */
    PCI_AHCI.vendor_id = PCI_AHCI_VENDOR_ID;
    PCI_AHCI.device_id = PCI_AHCI_DEVICE_ID;
    PCI_AHCI.MMIO_reg = PCI_AHCI_MMIO_REG;
    PCI_AHCI.interrupt_reg = PCI_AHCI_INT_REG;

    PCI_scan_bus(&PCI_AHCI);
    PCI_get_MMIO_space_size(&PCI_AHCI);
    // PCI_AHCI.start_virtual_address = (uint64_t) VMM_get_AHCI_MMIO_virt();
    
    // PCI_pages_for_ahci_start = (void*) VMM_get_AHCI_phys();

    AHCI_probe_port((HBA_MEM_t*) PCI_AHCI.start_virtual_address);
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

    // PCI_pages_for_ahci_start = VMM_get_AHCI_phys();
}