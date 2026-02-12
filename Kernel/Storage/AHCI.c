#include <Storage/AHCI.h>

#include <FileSystem/ext4.h>
#include <Storage/SATA.h>
#include <Util/Buffer.h>
#include <Storage/VFS.h>
#include <Memory/VMM.h>
#include <Memory/PMM.h>
#include <CPU/PCI.h>
#include <CPU/IO.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static const AHCI_device_t AHCI_devices[] =
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

static HBA_MEM_t* AHCI_base_address;

static uint32_t SATA_device_count = 0;
static HBA_PORT_t* BLOCK_DEVICES[AHCI_MAX_SLOT];

extern uintptr_t ROOT_DEV;

static int AHCI_build_prdt(HBA_CMD_TBL_t* cmd_tbl, uintptr_t buf, uint32_t byte_count, uint16_t* prdt_out)
{
    uint32_t remaining = byte_count;
    uint16_t prdt = 0;

    while (remaining > 0)
    {
        if (prdt >= 248)
            return SATA_IO_ERROR_NO_SLOT;

        uintptr_t phys = 0;
        if (!VMM_virt_to_phys(buf, &phys))
            return SATA_IO_ERROR_HUNG_PORT;

        uint32_t page_offset = (uint32_t) (phys & 0xFFF);
        uint32_t chunk = PHYS_PAGE_SIZE - page_offset;
        if (chunk > remaining)
            chunk = remaining;

        cmd_tbl->prdt_entry[prdt].dba = ADDRLO(phys);
        cmd_tbl->prdt_entry[prdt].dbau = ADDRHI(phys);
        cmd_tbl->prdt_entry[prdt].dbc = chunk - 1;
        cmd_tbl->prdt_entry[prdt].i = 1;

        buf += chunk;
        remaining -= chunk;
        prdt++;
    }

    *prdt_out = prdt;
    return SATA_IO_SUCCESS;
}

void AHCI_try_setup_device(uint16_t bus, uint32_t slot, uint16_t function, uint16_t vendor, uint16_t device)
{
    uint32_t bar5 = PCI_config_read(bus, slot, function, PCI_BAR5_ADDR_REG);
    if (bar5 == 0 || bar5 == 0xFFFFFFFF)
        return;

    uintptr_t AHCI_base = (uintptr_t) (bar5 & ~0xFUL);
    if (AHCI_base == 0)
        return;

    const char* name = NULL;
    bool identified = false;

    for (uint16_t i = 0; AHCI_devices[i].vendor != 0 && !identified; i++)
    {
        if (AHCI_devices[i].vendor == vendor && AHCI_devices[i].device == device)
        {
            name = AHCI_devices[i].name;
            identified = true;
        }
    }

    if (identified)
    {
        AHCI_base_address = (HBA_MEM_t*) VMM_get_AHCI_virt();
        VMM_map_pages((uintptr_t) AHCI_base_address, AHCI_base, AHCI_MEM_LENGTH);
        AHCI_try_setup_known_device((char*) name, AHCI_base_address, bus, slot, function);
    }
}

int AHCI_get_device_count(void)
{
    return (int) SATA_device_count;
}

HBA_PORT_t* AHCI_get_device(int index)
{
    if (index < 0 || index >= (int) SATA_device_count)
        return NULL;
    return BLOCK_DEVICES[index];
}

void AHCI_try_setup_known_device(char* name, HBA_MEM_t* AHCI, uint16_t bus, uint16_t slot, uint16_t function)
{
    printf("%s controller found (bus=%d, slot=%d, function=%d, ABAR=%llX).\n", name, bus, slot, function, AHCI);
    printf("HBA is in %s mode.\n", (AHCI->ghc == 0 ? "legacy" : "AHCI-only"));

    uint32_t pi = AHCI->pi;
    for (int i = 0; i < 32; i++)
    {
        uint32_t port_mask = 1U << i;
        if ((pi & port_mask) == 0)
            continue;

        HBA_PORT_t* HBA_port = (HBA_PORT_t*) &AHCI->ports[i];
        if (HBA_port->sig == SATA_SIG_ATAPI || HBA_port->sig == SATA_SIG_SEMB || HBA_port->sig == SATA_SIG_PM)
            continue;

        uint32_t ssts = HBA_port->ssts;
        uint8_t ipm = (ssts >> 8) & 0x0F;
        uint8_t det = ssts & 0x07; // The device detection (DET) flags are the bottom 3 bits.
        if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE)
            continue;

        printf("SATA device detected:\n");
        AHCI_SATA_init(HBA_port, i);
    }
}

void AHCI_SATA_init(HBA_PORT_t* port, int num)
{
    if (AHCI_rebase_port(port, num))
    {
        uint8_t buf[512];
        memset(buf, 0xFF, sizeof (buf));

        int result = AHCI_sata_read(port, 1, 0, 1, buf);
        if (result == SATA_IO_SUCCESS)
        {
            if (SATA_device_count >= AHCI_MAX_SLOT)
            {
                printf("\tInit failure: AHCI device table is full.\n");
                return;
            }

            uint32_t dev_num = SATA_device_count++;
            printf("\tInit success: disk(%d, %d) !\n", DEV_SATA, dev_num);
            BLOCK_DEVICES[dev_num] = port;
            if (dev_num == 0)
                ROOT_DEV = TODEVNUM(DEV_SATA, 0);

            printf("\tThis disk %s ext4 !\n", ext4_check_format(port) ? "is" : "isn\'t");
        } else
            printf("\tInit failure !\n");
    }
}

bool AHCI_rebase_port(HBA_PORT_t* port, int num)
{
    (void) num;
    printf("\tRebasing port...\n");

    if (!AHCI_stop_port(port))
    {
        printf("\tFAILED !\n");
        return false;
    }

    void* clb_virt = PMM_alloc_page();
    void* fb_virt = PMM_alloc_page();
    if (!clb_virt || !fb_virt)
        panic("SATA: failed to allocate CLB/FIS buffers !");

    uintptr_t clb_phys = 0;
    uintptr_t fb_phys = 0;
    if (!VMM_virt_to_phys((uintptr_t) clb_virt, &clb_phys))
        panic("SATA CLB is not mapped !");
    if (!VMM_virt_to_phys((uintptr_t) fb_virt, &fb_phys))
        panic("SATA FIS is not mapped !");

    port->clb = ADDRLO(clb_phys);
    port->clbu = ADDRHI(clb_phys);
    port->fb = ADDRLO(fb_phys);
    port->fbu = ADDRHI(fb_phys);

    port->serr = (uint32_t) -1; // Clear all pending errors.
    port->is = (uint32_t) -1;
    port->ie = 0;
    
    memset(clb_virt, 0, 1024);
    memset(fb_virt, 0, 256);

    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) clb_virt;
    for (uint8_t i = 0; i < 32; i++)
    {
        void* ct_virt = PMM_alloc_page();
        if (!ct_virt)
            panic("SATA CTBA allocation failed !");

        uintptr_t ct_phys = 0;
        if (!VMM_virt_to_phys((uintptr_t) ct_virt, &ct_phys))
            panic("SATA CTBA is not mapped !");

        cmd_header[i].prdtl = 8; // 8 prdt entries per command table.
        cmd_header[i].ctba = ADDRLO(ct_phys);
        cmd_header[i].ctbau = ADDRHI(ct_phys);
    
        memset(ct_virt, 0, 256);
    }

    AHCI_start_port(port);
    printf("\tDONE !\n");

    return true;
}

bool AHCI_stop_port(HBA_PORT_t* port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    uint32_t count = 0;
    while (count++ < SATA_IO_MAX_WAIT) // Wait until FR (bit 14), CR (bit 15) are cleared.
    {
        if (!(port->cmd & (HBA_PxCMD_CR | HBA_PxCMD_FR)))
        {
            port->cmd &= ~HBA_PxCMD_FRE; // Clear FRE (bit 4).
            return true;
        }

        __asm__ __volatile__ ("pause");
    }

    return false;
}

void AHCI_start_port(HBA_PORT_t* port)
{
    while (port->cmd & HBA_PxCMD_CR)
        __asm__ __volatile__ ("nop");

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;

    port->is = 0;
    port->ie = 0xFFFFFFFF;
}

int AHCI_find_cmdslot(HBA_PORT_t* port)
{
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < AHCI_MAX_SLOT; i++)
    {
        if ((slots & 1) == 0)
            return i;
        slots >>= 1;
    }
    return -1;
}

int AHCI_sata_read(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint8_t* buf) // TODO: FIXME
{
    port->is = (uint32_t) -1;  // Clear pending interrupt bits.
    int spin = 0;   // Spin lock timeout counter.

    if (count == 0)
        return SATA_IO_SUCCESS;

    int slot = AHCI_find_cmdslot(port);
    if (slot == -1)
        return SATA_IO_ERROR_NO_SLOT;

    uintptr_t clb_phys = HILO2ADDR(port->clbu, port->clb);
    uintptr_t clb_virt = 0;
    if (!VMM_phys_to_virt_identity(clb_phys, &clb_virt))
        panic("SATA CLB is not identity-mapped !");
    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) clb_virt;

    cmd_header += slot;
    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 0; // Read from device.
    cmd_header->prdbc = 0;

    uintptr_t ctba_phys = HILO2ADDR(cmd_header->ctbau, cmd_header->ctba);
    uintptr_t ctba_virt = 0;
    if (!VMM_phys_to_virt_identity(ctba_phys, &ctba_virt))
        panic("SATA CTBA is not identity-mapped !");
    HBA_CMD_TBL_t* cmd_tbl = (HBA_CMD_TBL_t*) ctba_virt;
    memset(cmd_tbl, 0, PHYS_PAGE_SIZE);

    uint32_t byte_count = count * AHCI_SECTOR_SIZE;
    uint16_t prdtl = 0;
    int prdt_status = AHCI_build_prdt(cmd_tbl, (uintptr_t) buf, byte_count, &prdtl);
    if (prdt_status != SATA_IO_SUCCESS)
        return prdt_status;

    cmd_header->prdtl = prdtl;

    // Setup command.
    FIS_REG_H2D_t* cmd_fis = (FIS_REG_H2D_t*) (&cmd_tbl->cfis);

    cmd_fis->fis_type = FIS_TYPE_REG_H2D;
    cmd_fis->c = 1; // Command.
    cmd_fis->command = ATA_CMD_READ_DMA_EX;

    cmd_fis->lba0 = (uint8_t) startl;
    cmd_fis->lba1 = (uint8_t) (startl >> 8);
    cmd_fis->lba2 = (uint8_t) (startl >> 16);
    cmd_fis->device = FIS_LBA_MODE;
    cmd_fis->lba3 = (uint8_t) (startl >> 24);
    cmd_fis->lba4 = (uint8_t) starth;
    cmd_fis->lba5 = (uint8_t) (starth >> 8);
    cmd_fis->countl = count & 0xFF;
    cmd_fis->counth = (count >> 8) & 0xFF;

    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < SATA_IO_MAX_WAIT) // Wait until the port is no longer busy.
    {
        spin++;
        __asm__ __volatile__ ("pause");
    }

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    port->ci = 1 << slot; // Issue command.

    spin = 0;
    while ((port->ci & (1 << slot)) && spin < SATA_IO_MAX_WAIT)
    {
        if (port->is & HBA_PxIS_TFES)
            return SATA_IO_ERROR_HUNG_PORT;
        spin++;
        __asm__ __volatile__ ("pause");
    }

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    return SATA_IO_SUCCESS;
}

int AHCI_SATA_write(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint8_t* buf)
{
    if (count == 0)
        return SATA_IO_SUCCESS;

    port->is = (uint32_t) -1;

    int slot = AHCI_find_cmdslot(port);
    if (slot == -1)
        return SATA_IO_ERROR_NO_SLOT;

    uintptr_t clb_phys = HILO2ADDR(port->clbu, port->clb);
    uintptr_t clb_virt = 0;
    if (!VMM_phys_to_virt_identity(clb_phys, &clb_virt))
        panic("SATA CLB is not mapped !");
    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) clb_virt;
    cmd_header += slot;

    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 1; // Write to device.
    cmd_header->c = 1;
    cmd_header->prdbc = 0;

    uintptr_t ctba_phys = HILO2ADDR(cmd_header->ctbau, cmd_header->ctba);
    uintptr_t ctba_virt = 0;
    if (!VMM_phys_to_virt_identity(ctba_phys, &ctba_virt))
        panic("SATA CTBA is not mapped !");
    HBA_CMD_TBL_t* cmd_tbl = (HBA_CMD_TBL_t*) ctba_virt;
    memset(cmd_tbl, 0, PHYS_PAGE_SIZE);

    uint32_t byte_count = count * AHCI_SECTOR_SIZE;
    uint16_t prdtl = 0;
    int prdt_status = AHCI_build_prdt(cmd_tbl, (uintptr_t) buf, byte_count, &prdtl);
    if (prdt_status != SATA_IO_SUCCESS)
        return prdt_status;

    cmd_header->prdtl = prdtl;

    FIS_REG_H2D_t* cmd_fis = (FIS_REG_H2D_t*) (&cmd_tbl->cfis);

    cmd_fis->fis_type = FIS_TYPE_REG_H2D;
    cmd_fis->c = 1; // Cmd.
    cmd_fis->command = ATA_CMD_WRITE_DMA_EXT;

    cmd_fis->lba0 = (uint8_t) startl;
    cmd_fis->lba1 = (uint8_t) (startl >> 8);
    cmd_fis->lba2 = (uint8_t) (startl >> 16);
    cmd_fis->device = FIS_LBA_MODE;
    cmd_fis->lba3 = (uint8_t) (startl >> 24);
    cmd_fis->lba4 = (uint8_t) starth;
    cmd_fis->lba5 = (uint8_t) (starth >> 8);
    
    cmd_fis->countl = count & 0xFF;
    cmd_fis->counth = (count >> 8) & 0xFF;

    int spin = 0;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < SATA_IO_MAX_WAIT) // Wait until the port is no longer busy.
    {
        spin++;
        __asm__ __volatile__ ("pause");
    }

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    port->ci = 1 << slot; // Issue command.

    spin = 0;
    while ((port->ci & (1 << slot)) && spin < SATA_IO_MAX_WAIT)
    {
        if (port->is & HBA_PxIS_TFES)
            return SATA_IO_ERROR_HUNG_PORT;
        spin++;
        __asm__ __volatile__ ("pause");
    }

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    return SATA_IO_SUCCESS;
}
