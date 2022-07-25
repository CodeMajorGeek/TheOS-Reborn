#include <CPU/PCI.h>

#include <Debug/Assert.h>
#include <Storage/AHCI.h>
#include <CPU/IO.h>

#include <string.h>

void PCI_init(void)
{
    printf("Probing PCI !\n");

    static PCI_bus_t root_bus;
    memset(&root_bus, 0, sizeof (root_bus));

    PCI_scan_bus(&root_bus);
}

static void PCI_conf_set_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset)
{
    assert(bus < 256);
    assert(dev < 32);
    assert(func < 8);
    assert(offset < 256);
    assert((offset & 0x3) == 0);

    uint32_t v = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | offset;
    IO_outl(PCI_CONF_ADDR_IO_PORT, v);
}

uint32_t PCI_config_read(PCI_func_t* func, uint8_t offset)
{
    PCI_conf_set_addr(func->bus->bus_no, func->dev, func->func, offset);
    return IO_inl(PCI_CONF_DATA_IO_PORT);
}

bool PCI_scan_device(PCI_func_t* func)
{
    func->dev_id = PCI_config_read(func, PCI_ID_REG);
    if (PCI_VENDOR(func->dev_id) == 0xFFFF)
        return false;

    uint32_t intr = PCI_config_read(func, PCI_INTERRUPT_REG);
    func->IRQ_line = PCI_INTERRUPT_LINE(intr);

    func->dev_class = PCI_config_read(func, PCI_CLASS_REG);

    return true;
}

int PCI_scan_bus(PCI_bus_t* bus)
{
    int tdev = 0;

    PCI_func_t f;
    memset(&f, 0, sizeof (f));
    f.bus = bus;


    for (f.dev = 0; f.dev < 32; f.dev++)
    {
        uint32_t bhlc = PCI_config_read(&f, PCI_BHLC_REG);
        if (PCI_HDRTYPE_TYPE(bhlc) > 1)
            continue;

        ++tdev;

        for (f.func = 0; f.func < (PCI_HDRTYPE_MULTIFN(bhlc) ? 8 : 1); f.func++)
        {
            if (!PCI_scan_device(&f))
                continue;
            else
                PCI_try_attach(&f);
        }
    }
    
    return tdev;
}

static void PCI_try_attach(PCI_func_t* func)
{
    uint16_t dev_class = PCI_CLASS(func->dev_class);
    switch (dev_class)
    {
        case PCI_DEV_CLASS_STORAGE:
        case PCI_DEV_CLASS_BRIDGE:
            PCI_attach_storage_dev(func);
            break;
        default:
            break;
    }
}

static void PCI_attach_storage_dev(PCI_func_t* func)
{
    printf("Trying setup PCI bus no: %d, dev: %d, func: %d \n", func->bus->bus_no, func->dev, func->func);
    AHCI_try_setup_device(func->bus->bus_no, func->dev, func->func);
}