#include <CPU/PCI.h>

#include <CPU/IO.h>
#include <Debug/Assert.h>
#include <Debug/KDebug.h>
#include <Memory/VMM.h>
#include <Storage/AHCI.h>

static void PCI_try_attach(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_storage_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static bool PCI_read_bar_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t bar_index, uintptr_t* phys_out, bool* is_io_out, bool* is_64_out);
static uint16_t PCI_set_intx_disable(uint8_t bus, uint8_t slot, uint8_t function, bool disable);

void PCI_init(void)
{
    PCI_scan_bus(0);
}

uint8_t PCI_config_readb(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    return (uint8_t) ((PCI_config_read(bus, slot, function, offset) >> ((offset & 3U) * 8U)) & 0xFFU);
}

uint32_t PCI_config_read(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    assert(slot < 32);
    assert(function < 8);

    uint32_t lbus = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) function;

    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | (1U << 31));

    IO_outl(PCI_CONF_ADDR_IO_PORT, address);
    return IO_inl(PCI_CONF_DATA_IO_PORT);
}

uint16_t PCI_config_readw(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    return (uint16_t) ((PCI_config_read(bus, slot, function, offset) >> ((offset & 2U) * 8U)) & 0xFFFFU);
}

void PCI_config_write(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value)
{
    assert(slot < 32);
    assert(function < 8);

    uint32_t lbus = (uint32_t) bus;
    uint32_t lslot = (uint32_t) slot;
    uint32_t lfunc = (uint32_t) function;
    uint32_t address = (uint32_t) ((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | (1U << 31));

    IO_outl(PCI_CONF_ADDR_IO_PORT, address);
    IO_outl(PCI_CONF_DATA_IO_PORT, value);
}

void PCI_config_writew(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value)
{
    uint8_t aligned = offset & 0xFCU;
    uint32_t shift = (uint32_t) ((offset & 2U) * 8U);
    uint32_t mask = 0xFFFFU << shift;
    uint32_t current = PCI_config_read(bus, slot, function, aligned);
    uint32_t next = (current & ~mask) | (((uint32_t) value) << shift);
    PCI_config_write(bus, slot, function, aligned, next);
}

void PCI_config_writeb(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint8_t value)
{
    uint8_t aligned = offset & 0xFCU;
    uint32_t shift = (uint32_t) ((offset & 3U) * 8U);
    uint32_t mask = 0xFFU << shift;
    uint32_t current = PCI_config_read(bus, slot, function, aligned);
    uint32_t next = (current & ~mask) | (((uint32_t) value) << shift);
    PCI_config_write(bus, slot, function, aligned, next);
}

uint8_t PCI_find_capability(uint8_t bus, uint8_t slot, uint8_t function, uint8_t cap_id)
{
    uint16_t status = PCI_config_readw(bus, slot, function, PCI_STATUS_REG);
    if ((status & PCI_STATUS_CAP_LIST) == 0)
        return 0;

    uint8_t ptr = PCI_config_readb(bus, slot, function, PCI_CAP_PTR_REG) & 0xFCU;
    for (uint8_t guard = 0; guard < 48 && ptr >= 0x40; guard++)
    {
        uint8_t current_id = PCI_config_readb(bus, slot, function, ptr + 0);
        uint8_t next = PCI_config_readb(bus, slot, function, ptr + 1) & 0xFCU;
        if (current_id == cap_id)
            return ptr;
        if (next == ptr)
            break;
        ptr = next;
    }

    return 0;
}

static bool PCI_read_bar_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t bar_index, uintptr_t* phys_out, bool* is_io_out, bool* is_64_out)
{
    if (!phys_out || bar_index > 5)
        return false;

    uint8_t bar_offset = (uint8_t) (PCI_BAR0_ADDR_REG + (bar_index * 4U));
    uint32_t bar_lo = PCI_config_read(bus, slot, function, bar_offset);
    if (bar_lo == 0 || bar_lo == 0xFFFFFFFFU)
        return false;

    bool is_io = (bar_lo & 0x1U) != 0;
    bool is_64 = false;
    uintptr_t phys = 0;

    if (is_io)
    {
        phys = (uintptr_t) (bar_lo & ~0x3U);
    }
    else
    {
        uint32_t type = (bar_lo >> 1) & 0x3U;
        is_64 = (type == 0x2U);
        if (is_64)
        {
            if (bar_index >= 5)
                return false;
            uint32_t bar_hi = PCI_config_read(bus, slot, function, (uint8_t) (bar_offset + 4));
            phys = (((uintptr_t) bar_hi) << 32) | (uintptr_t) (bar_lo & ~0xFULL);
        }
        else
        {
            phys = (uintptr_t) (bar_lo & ~0xFULL);
        }
    }

    *phys_out = phys;
    if (is_io_out)
        *is_io_out = is_io;
    if (is_64_out)
        *is_64_out = is_64;
    return true;
}

bool PCI_enable_msi(uint8_t bus, uint8_t slot, uint8_t function, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t cap = PCI_find_capability(bus, slot, function, PCI_CAP_ID_MSI);
    if (cap == 0)
        return false;

    uint16_t ctrl = PCI_config_readw(bus, slot, function, (uint8_t) (cap + 2));
    bool is_64 = (ctrl & PCI_MSI_CTRL_64BIT_CAPABLE) != 0;

    uint32_t msg_addr_lo = PCI_MSI_ADDR_BASE | (((uint32_t) dest_apic_id & 0xFFU) << 12);
    uint16_t msg_data = (uint16_t) vector;

    PCI_config_write(bus, slot, function, (uint8_t) (cap + 4), msg_addr_lo);
    if (is_64)
    {
        PCI_config_write(bus, slot, function, (uint8_t) (cap + 8), 0);
        PCI_config_writew(bus, slot, function, (uint8_t) (cap + 12), msg_data);
    }
    else
    {
        PCI_config_writew(bus, slot, function, (uint8_t) (cap + 8), msg_data);
    }

    ctrl &= (uint16_t) ~PCI_MSI_CTRL_MULTI_MSG_EN_MASK;
    ctrl |= PCI_MSI_CTRL_MSI_ENABLE;
    PCI_config_writew(bus, slot, function, (uint8_t) (cap + 2), ctrl);

    uint16_t command = PCI_set_intx_disable(bus, slot, function, true);
    kdebug_printf("[PCI] MSI enabled b=%u s=%u f=%u cmd=0x%X intx=%s\n",
                  (unsigned) bus,
                  (unsigned) slot,
                  (unsigned) function,
                  command,
                  (command & PCI_COMMAND_INTX_DISABLE) ? "disabled" : "enabled");
    return true;
}

bool PCI_enable_msix(uint8_t bus, uint8_t slot, uint8_t function, uintptr_t mmio_bar5_phys, uintptr_t mmio_bar5_virt, size_t mmio_bar5_len, uint8_t vector, uint8_t dest_apic_id)
{
    uint8_t cap = PCI_find_capability(bus, slot, function, PCI_CAP_ID_MSIX);
    if (cap == 0)
        return false;

    uint16_t ctrl = PCI_config_readw(bus, slot, function, (uint8_t) (cap + 2));
    uint16_t table_size = (uint16_t) ((ctrl & PCI_MSIX_CTRL_TABLE_SIZE_MASK) + 1U);
    if (table_size == 0)
        return false;

    uint32_t table_info = PCI_config_read(bus, slot, function, (uint8_t) (cap + 4));
    uint8_t table_bir = (uint8_t) (table_info & PCI_MSIX_TABLE_BIR_MASK);
    uint32_t table_off = table_info & PCI_MSIX_TABLE_OFFSET_MASK;

    uintptr_t bar_phys = 0;
    bool bar_io = false;
    if (!PCI_read_bar_address(bus, slot, function, table_bir, &bar_phys, &bar_io, NULL) || bar_io)
        return false;

    uintptr_t table_phys = bar_phys + (uintptr_t) table_off;
    size_t table_len = (size_t) table_size * sizeof(PCI_msix_entry_t);
    if (table_len < sizeof(PCI_msix_entry_t))
        return false;

    uintptr_t table_virt = 0;
    if (table_bir == 5 &&
        mmio_bar5_virt != 0 &&
        mmio_bar5_phys != 0 &&
        table_phys >= mmio_bar5_phys &&
        (table_phys + table_len) <= (mmio_bar5_phys + mmio_bar5_len))
    {
        table_virt = mmio_bar5_virt + (table_phys - mmio_bar5_phys);
    }
    else
    {
        uintptr_t map_phys_page = table_phys & ~(uintptr_t) 0xFFFULL;
        uintptr_t map_page_off = table_phys & (uintptr_t) 0xFFFULL;
        size_t map_len = (size_t) map_page_off + table_len;
        VMM_map_mmio_uc_pages(PCI_MSIX_SCRATCH_VIRT_BASE, map_phys_page, map_len);
        table_virt = PCI_MSIX_SCRATCH_VIRT_BASE + map_page_off;
    }

    if (table_virt == 0)
        return false;

    PCI_msix_entry_t* table = (PCI_msix_entry_t*) table_virt;
    uint32_t msg_addr_lo = PCI_MSI_ADDR_BASE | (((uint32_t) dest_apic_id & 0xFFU) << 12);
    uint32_t msg_data = (uint32_t) vector;

    uint16_t masked_ctrl = (uint16_t) (ctrl | PCI_MSIX_CTRL_FUNCTION_MASK);
    PCI_config_writew(bus, slot, function, (uint8_t) (cap + 2), masked_ctrl);

    for (uint16_t i = 0; i < table_size; i++)
        table[i].vector_ctrl = PCI_MSIX_VECTOR_MASK_BIT;

    table[0].msg_addr_lo = msg_addr_lo;
    table[0].msg_addr_hi = 0;
    table[0].msg_data = msg_data;
    table[0].vector_ctrl = 0;

    uint16_t enabled_ctrl = (uint16_t) ((masked_ctrl & ~PCI_MSIX_CTRL_FUNCTION_MASK) | PCI_MSIX_CTRL_ENABLE);
    PCI_config_writew(bus, slot, function, (uint8_t) (cap + 2), enabled_ctrl);

    uint16_t command = PCI_set_intx_disable(bus, slot, function, true);
    kdebug_printf("[PCI] MSI-X enabled b=%u s=%u f=%u cmd=0x%X intx=%s\n",
                  (unsigned) bus,
                  (unsigned) slot,
                  (unsigned) function,
                  command,
                  (command & PCI_COMMAND_INTX_DISABLE) ? "disabled" : "enabled");
    return true;
}

static uint16_t PCI_set_intx_disable(uint8_t bus, uint8_t slot, uint8_t function, bool disable)
{
    uint16_t command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
    if (disable)
        command |= PCI_COMMAND_INTX_DISABLE;
    else
        command &= (uint16_t) ~PCI_COMMAND_INTX_DISABLE;

    PCI_config_writew(bus, slot, function, PCI_COMMAND_REG, command);
    return PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
}

void PCI_check_device(uint8_t bus, uint8_t slot)
{
    uint8_t function = 0;

    uint16_t vendor_id = PCI_config_readw(bus, slot, function, PCI_VENDOR_REG);
    if (vendor_id == 0xFFFF)
        return; // No device attached.

    uint16_t device_id = PCI_config_readw(bus, slot, function, PCI_DEVICE_REG);
    kdebug_printf("[PCI] b=%u s=%u f=%u vid=0x%X did=0x%X\n", bus, slot, function, vendor_id, device_id);

    PCI_try_attach(bus, slot, function, vendor_id, device_id);

    PCI_check_function(bus, slot, function);
    uint8_t header_type = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_BIST_REG) & 0xFF);
    if ((header_type & 0x80U) != 0) // It's a multi function device.
    {
        for (function = 1; function < 8; function++)
            PCI_check_function(bus, slot, function);
    }
}

void PCI_check_function(uint8_t bus, uint8_t slot, uint8_t function)
{
    uint8_t base_class = (uint8_t) ((PCI_config_readw(bus, slot, function, PCI_CLASS_REG) >> 8) & 0xFF);
    uint8_t sub_class = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_CLASS_REG) & 0xFF);

    if (base_class == 0x6 && sub_class == 0x4)
    {
        uint8_t secondary_bus = (uint8_t) (PCI_config_readw(bus, slot, function, PCI_SECONDARY_BUS_REG) & 0xFF);
        PCI_scan_bus(secondary_bus);
    }
}

void PCI_scan_bus(uint8_t bus)
{
    for (uint8_t slot = 0; slot < 32; slot++)
        PCI_check_device(bus, slot);
}

static void PCI_try_attach(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device)
{
    uint8_t base_class = (uint8_t) ((PCI_config_readw(bus, slot, function, PCI_CLASS_REG) >> 8) & 0xFF);
    kdebug_printf("[PCI] class b=%u s=%u f=%u class=0x%X\n", bus, slot, function, base_class);
    switch (base_class)
    {
        case PCI_DEV_CLASS_STORAGE:
        case PCI_DEV_CLASS_BRIDGE:
            PCI_attach_storage_dev(bus, slot, function, vendor, device);
            break;
        default:
            break;
    }
}

static void PCI_attach_storage_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device)
{
    AHCI_try_setup_device(bus, slot, function, vendor, device);
}
