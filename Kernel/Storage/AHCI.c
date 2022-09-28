#include <Storage/AHCI.h>

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

void AHCI_try_setup_device(uint16_t bus, uint32_t slot, uint16_t function, uint16_t vendor, uint16_t device)
{
    uintptr_t AHCI_base = (uintptr_t) PCI_config_read(bus, slot, function, PCI_BAR5_ADDR_REG);

    if (AHCI_base != 0 && AHCI_base != 0xFFFFFFFF)
    {
        const char* name;
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

            VMM_map_pages(AHCI_base, AHCI_base, AHCI_MEM_LENGTH);
            AHCI_try_setup_known_device((char*) name, (HBA_MEM_t*) AHCI_base, bus, slot, function);
        }
    }
}

void AHCI_try_setup_known_device(char* name, HBA_MEM_t* AHCI, uint16_t bus, uint16_t slot, uint16_t function)
{
    printf("%s controller found (bus=%d, slot=%d, function=%d, ABAR=0x%X).\n", name, bus, slot, function, AHCI);
    printf("HBA is in %s mode.\n", (AHCI->ghc == 0 ? "legacy" : "AHCI-only"));

    uint64_t pi = AHCI->pi;
    for (int i = 0; i != 32; i++)
    {
        uint64_t port_mask = 1 << i;
        if ((pi & port_mask) == 0)
            continue;

        HBA_PORT_t* HBA_port = (HBA_PORT_t*) &AHCI->ports[i];
        if (HBA_port->sig != SATA_SIG_ATAPI && HBA_port->sig != SATA_SIG_SEMB && HBA_port->sig != SATA_SIG_PM)
        {
            uint64_t ssts = HBA_port->ssts;
        
            uint8_t ipm = (ssts >> 8) & 0x0F;
            uint8_t spd = (ssts >> 4) & 0x0F;
            uint8_t det = ssts & 0x07; // The device detection (DET) flags are the bottom 3 bits.

            if (det != HBA_PORT_DET_PRESENT && ipm != HBA_PORT_IPM_ACTIVE)
                ; // NOPE !
            else if (HBA_port->sig == SATA_SIG_ATAPI)
                ; // ATAPI Device.
            else if (HBA_port->sig == SATA_SIG_SEMB)
                ; 
            else if (HBA_port->sig == SATA_SIG_PM)
                ; // Port multiplier detected.
            else
            {
                printf("SATA device detected:\n");
                printf("\tport[%d].sig = 0x%X.\n", i, HBA_port->sig);
                printf("\tipm=0x%X, spd=0x%X, det=0x%X\n", ipm, spd, det);

                AHCI_SATA_init(HBA_port, i);
            }
        }
    }
}

void AHCI_SATA_init(HBA_PORT_t* port, int num)
{
    if (AHCI_rebase_port(port, num))
    {
        uint8_t buf[512];
        memset(buf, 0xFF, sizeof (buf));

        int result = AHCI_sata_read(port, 2, 0, 1, buf);
        if (result == SATA_IO_SUCCESS)
        {
            uint32_t dev_num = SATA_device_count++;
            printf("\tInit success: disk(%d, %d) !\n", DEV_SATA, 0);
            BLOCK_DEVICES[dev_num] = port;
            if (dev_num == 0)
                ROOT_DEV = TODEVNUM(DEV_SATA, 0);

            printf("0x400 : 0x%X%X\n", buf[0], buf[1]);
        } else
            printf("\tInit failure !\n");
    }
}

bool AHCI_rebase_port(HBA_PORT_t* port, int num)
{
    printf("\tRebasing port...\n");

    if (!AHCI_stop_port(port))
    {
        printf("\tFAILED !\n");
        return false;
    }

    uintptr_t AHCI_base = (uintptr_t) AHCI_base_address + (num << 8); // Port base + 1MB/port.
    port->clb = ADDRLO(AHCI_base);
    port->clbu = ADDRHI(AHCI_base);

    uintptr_t FB_addr = (uintptr_t) AHCI_base_address + (32 << 10) + (num << 8);
    port->fb = ADDRLO(FB_addr);
    port->fbu = ADDRHI(FB_addr);

    port->serr = 1; // For each implemented port, clear the PxSERR register by writting 1 to each implement location.
    port->is = 0;
    port->ie = 1;
    
    memset((void*) AHCI_base, 0, 1024);
    memset((void*) FB_addr, 0, 256);

    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) AHCI_base;
    for (uint8_t i = 0; i < 32; i++)
    {
        uintptr_t CT_addr = (uintptr_t) AHCI_base_address + (40 << 10) + (num << 13) + (i << 8);

        cmd_header[i].prdtl = 8; // 8 prdt entries per command table.
        cmd_header[i].ctba = ADDRLO(CT_addr);
        cmd_header[i].ctbau = ADDRHI(CT_addr);
    
        memset((void*) CT_addr, 0, 256);
    }

    AHCI_start_port(port);
    printf("\tDONE !\n");

    return true;
}

bool AHCI_stop_port(HBA_PORT_t* port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    uint16_t count = 0;
    do // Wait until FR (bit 14), CR (bit 15) are cleared.
    {
        if (!(port->cmd & (HBA_PxCMD_CR | HBA_PxCMD_FR)))
            break;
    }
    while (count++ < 1000);
    
    port->cmd &= ~HBA_PxCMD_FRE; // Clear FRE (bit 4).
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
    port->is = -1;  // Clear pending interrupt bits.
    int spin = 0;   // Spin lock timeout counter.

    int slot = AHCI_find_cmdslot(port);
    if (slot == -1)
        return SATA_IO_ERROR_NO_SLOT;

    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) P2V(HILO2ADDR(port->clbu, port->clb));

    cmd_header += slot;
    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 0; // Read from device.
    cmd_header->prdtl = (uint16_t) ((count - 1) >> 4) + 1; // PRDT entries count.

    HBA_CMD_TBL_t* cmd_tbl = (HBA_CMD_TBL_t*) P2V(HILO2ADDR(cmd_header->ctbau, cmd_header->ctba));
    memset(cmd_tbl, 0, sizeof (HBA_CMD_TBL_t) + (cmd_header->prdtl - 1) * sizeof (HBA_PRDT_ENTRY_t));

    uintptr_t addr = V2P(buf);
    if (addr & 0x1)
        panic("SATA CBA address not word aligned !");
    
    int i;
    for (i = 0; i < cmd_header->prdtl - 1; i++)
    {
        panic("Unsupported read request: only sector reads are supported !"); // TODO: implement multi sectors read.
        cmd_tbl->prdt_entry[i].dba = ADDRLO(addr);
        cmd_tbl->prdt_entry[i].dbau = ADDRHI(addr);
        cmd_tbl->prdt_entry[i].dbc = 8 * 1024 - 1;
        cmd_tbl->prdt_entry[i].i = 1;
        buf += 4 * 1024;    // 4K words.
        count -= 16;        // 16 sectors.
    }

    // Last entry.
    cmd_tbl->prdt_entry[i].dba = ADDRLO(addr);
    cmd_tbl->prdt_entry[i].dbau = ADDRHI(addr);
    cmd_tbl->prdt_entry[i].dbc = 512 - 1;
    cmd_tbl->prdt_entry[i].i = 1;

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
        spin++; 

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    port->ci = 1 << slot; // Issue command.

    return SATA_IO_SUCCESS;
}

int AHCI_SATA_write(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint8_t* buf)
{
    port->is = (uint32_t) -1;

    uint32_t slot = AHCI_find_cmdslot(port);
    if (slot == -1)
        return SATA_IO_ERROR_NO_SLOT;

    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) P2V(HILO2ADDR(port->clbu, port->clb));
    cmd_header += slot;

    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 1; // Write to device.
    cmd_header->c = 1;
    cmd_header->prdtl = 1; // PRDT entries count.

    HBA_CMD_TBL_t* cmd_tbl = (HBA_CMD_TBL_t*) P2V(HILO2ADDR(cmd_header->ctbau, cmd_header->ctba));
    memset(cmd_tbl, 0, sizeof (HBA_CMD_TBL_t) + (cmd_header->prdtl - 1) * sizeof (HBA_PRDT_ENTRY_t));

    uint64_t addr = V2P(buf);
    if (addr & 0x1)
        panic("SATA CBA address isn't word aligned !");

    cmd_tbl->prdt_entry[0].dba = ADDRLO(addr);
    cmd_tbl->prdt_entry[0].dbau = ADDRHI(addr);
    cmd_tbl->prdt_entry[0].dbc = 511; // 512 bytes per sector.
    cmd_tbl->prdt_entry[0].i = 0;

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
    
    cmd_fis->countl = 2;
    cmd_fis->counth = 0;

    int spin = 0;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < SATA_IO_MAX_WAIT) // Wait until the port is no longer busy.
        spin++; 

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    port->ci = 1 << slot; // Issue command.

    return SATA_IO_SUCCESS;
}