#include <Device/AHCI.h>

static HBA_MEM_t* abar;

void AHCI_init(void)
{
    ISR_register_IRQ(IRQ10, AHCI_callback);
}

void AHCI_probe_port(HBA_MEM_t* abar_temp)
{
	// Search disk in implemented ports
	uint32_t pi = abar_temp->pi;
	int i = 0;
	while (i < 32)
	{
		if (pi & 1)
		{
			int dt = AHCI_check_type(&abar_temp->ports[i]);
			if (dt == AHCI_DEV_SATA)
            {
                trace_ahci("SATA drive found at port %d\n", i);
                abar = abar_temp;
                AHCI_port_rebase(abar->ports, i);
                trace_ahci("DONE AHCI INITIALISATION :: PORT REBASE");
            }
			else if (dt == AHCI_DEV_SATAPI)
				trace_ahci("SATAPI drive found at port %d\n", i);
			else if (dt == AHCI_DEV_SEMB)
				trace_ahci("SEMB drive found at port %d\n", i);
			else if (dt == AHCI_DEV_PM)
				trace_ahci("PM drive found at port %d\n", i);
			else
				trace_ahci("No drive found at port %d\n", i);
		}
 
		pi >>= 1;
		i++;
	}
}

// Check device type
static int AHCI_check_type(HBA_PORT_t* port)
{
	uint32_t ssts = port->ssts;
 
	uint8_t ipm = (ssts >> 8) & 0x0F;
	uint8_t det = ssts & 0x0F;
 
	if (det != HBA_PORT_DET_PRESENT)	// Check drive status
		return AHCI_DEV_NULL;
	if (ipm != HBA_PORT_IPM_ACTIVE)
		return AHCI_DEV_NULL;
 
	switch (port->sig)
	{
	case SATA_SIG_ATAPI:
		return AHCI_DEV_SATAPI;
	case SATA_SIG_SEMB:
		return AHCI_DEV_SEMB;
	case SATA_SIG_PM:
		return AHCI_DEV_PM;
	default:
		return AHCI_DEV_SATA;
	}
}

void AHCI_port_rebase(HBA_PORT_t* port, int portno)
{
	AHCI_stop_cmd(port);	// Stop command engine
 
	// Command list offset: 1K*portno
	// Command list entry size = 32
	// Command list entry maxim count = 32
	// Command list maxim size = 32*32 = 1K per port
	port->clb = AHCI_BASE + (portno<<10);
	port->clbu = 0;
	memset((void*) (port->clb), 0, 1024);
 
	// FIS offset: 32K+256*portno
	// FIS entry size = 256 bytes per port
	port->fb = AHCI_BASE + (32<<10) + (portno<<8);
	port->fbu = 0;
	memset((void*)(port->fb), 0, 256);
 
	// Command table offset: 40K + 8K*portno
	// Command table size = 256*32 = 8K per port
	HBA_CMD_HEADER_t* cmdheader = (HBA_CMD_HEADER_t*)(port->clb);
	for (int i=0; i<32; i++)
	{
		cmdheader[i].prdtl = 8;	// 8 prdt entries per command table
					// 256 bytes per command table, 64+16+48+16*8
		// Command table offset: 40K + 8K*portno + cmdheader_index*256
		cmdheader[i].ctba = AHCI_BASE + (40<<10) + (portno<<13) + (i<<8);
		cmdheader[i].ctbau = 0;
		memset((void*)cmdheader[i].ctba, 0, 256);
	}
 
	AHCI_start_cmd(port);	// Start command engine
}
 
// Start command engine
void AHCI_start_cmd(HBA_PORT_t* port)
{
	// Wait until CR (bit15) is cleared
	while (port->cmd & HBA_PxCMD_CR)
		;
 
	// Set FRE (bit4) and ST (bit0)
	port->cmd |= HBA_PxCMD_FRE;
	port->cmd |= HBA_PxCMD_ST; 
}
 
// Stop command engine
void AHCI_stop_cmd(HBA_PORT_t* port)
{
	// Clear ST (bit0)
	port->cmd &= ~HBA_PxCMD_ST;
 
	// Clear FRE (bit4)
	port->cmd &= ~HBA_PxCMD_FRE;
 
	// Wait until FR (bit14), CR (bit15) are cleared
	while(TRUE)
	{
		if (port->cmd & HBA_PxCMD_FR)
			continue;
		if (port->cmd & HBA_PxCMD_CR)
			continue;
		break;
	}
}

bool AHCI_read(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint16_t* buf)
{
	port->is = (uint32_t) -1;		// Clear pending interrupt bits
	int spin = 0; // Spin lock timeout counter
	int slot = AHCI_find_cmdslot(port);
	if (slot == -1)
		return false;
 
	HBA_CMD_HEADER_t* cmdheader = (HBA_CMD_HEADER_t*) port->clb;
	cmdheader += slot;
	cmdheader->cfl = sizeof(FIS_REG_H2D_t)/sizeof(uint32_t);	// Command FIS size
	cmdheader->w = 0;		// Read from device
	cmdheader->prdtl = (uint16_t)((count-1)>>4) + 1;	// PRDT entries count
 
	HBA_CMD_TBL_t* cmdtbl = (HBA_CMD_TBL_t*) (cmdheader->ctba);
	memset(cmdtbl, 0, sizeof (HBA_CMD_TBL_t) + (cmdheader->prdtl - 1) * sizeof (HBA_PRDT_ENTRY_t));
 
	// 8K bytes (16 sectors) per PRDT
    int i = 0;
	for (; i < cmdheader->prdtl - 1; i++)
	{
		cmdtbl->prdt_entry[i].dba = (uint32_t) buf;
		cmdtbl->prdt_entry[i].dbc = 8*1024-1;	// 8K bytes (this value should always be set to 1 less than the actual value)
		cmdtbl->prdt_entry[i].i = 1;
		buf += 4*1024;	// 4K words
		count -= 16;	// 16 sectors
	}
	// Last entry
	cmdtbl->prdt_entry[i].dba = (uint32_t) buf;
	cmdtbl->prdt_entry[i].dbc = (count<<9)-1;	// 512 bytes per sector
	cmdtbl->prdt_entry[i].i = 1;
 
	// Setup command
	FIS_REG_H2D_t* cmdfis = (FIS_REG_H2D_t*) (&cmdtbl->cfis);
 
	cmdfis->fis_type = FIS_TYPE_REG_H2D;
	cmdfis->c = 1;	// Command
	cmdfis->command = ATA_CMD_READ_DMA_EX;
 
	cmdfis->lba0 = (uint8_t) startl;
	cmdfis->lba1 = (uint8_t) (startl >> 8);
	cmdfis->lba2 = (uint8_t) (startl >> 16);
	cmdfis->device = 1 << 6;	// LBA mode
 
	cmdfis->lba3 = (uint8_t) (startl >> 24);
	cmdfis->lba4 = (uint8_t) starth;
	cmdfis->lba5 = (uint8_t) (starth >> 8);
 
	cmdfis->countl = count & 0xFF;
	cmdfis->counth = (count >> 8) & 0xFF;
 
	// The below loop waits until the port is no longer busy before issuing a new command
	while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000)
	{
		spin++;
	}
	if (spin == 1000000)
	{
		trace_ahci("Port is hung\n");
		return FALSE;
	}
 
	port->ci = 1<<slot;	// Issue command
 
	// Wait for completion
	while (TRUE)
	{
		// In some longer duration reads, it may be helpful to spin on the DPS bit 
		// in the PxIS port field as well (1 << 5)
		if ((port->ci & (1<<slot)) == 0) 
			break;
		if (port->is & HBA_PxIS_TFES)	// Task file error
		{
			trace_ahci("Read disk error\n");
			return FALSE;
		}
	}
 
	// Check again
	if (port->is & HBA_PxIS_TFES)
	{
		trace_ahci("Read disk error\n");
		return FALSE;
	}
 
	return true;
}
 
// Find a free command list slot
int AHCI_find_cmdslot(HBA_PORT_t* port)
{
	// If not set in SACT and CI, the slot is free
	uint32_t slots = (port->sact | port->ci);
    int cmdslots = (abar->cap & 0x0f00) >> 8; // Bit 8-12

	for (int i = 0; i < cmdslots; i++)
	{
		if ((slots & 1) == 0)
			return i;
		slots >>= 1;
	}
	trace_ahci("Cannot find free command list entry\n");
	return -1;
}

static void AHCI_callback(interrupt_frame_t* frame)
{
    trace_ahci("AHCI INTERRUPT TRIGGERED !\n");

    if (abar->ports[0].is & HBA_PxIS_TFES)
        trace_ahci("Read disk error !\n");

    trace_ahci("\tTFD=[%d],\n", ((HBA_PORT_t*) &abar->ports[0])->tfd);
    trace_ahci("\tSSTS=[%d],\n", ((HBA_PORT_t*) &abar->ports[0])->ssts);
    trace_ahci("\tIE=[%d],\n", ((HBA_PORT_t*) &abar->ports[0])->ie);
    trace_ahci("\tSERR=[%d],\n", ((HBA_PORT_t*) &abar->ports[0])->serr);
    trace_ahci("\tIS=[%d]\n", ((HBA_PORT_t*) &abar->ports[0])->is);

    abar->ports[0].is = 0xFFFF;

    while (TRUE);
}