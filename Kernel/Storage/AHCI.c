#include <Storage/AHCI.h>

#include <FileSystem/ext4.h>
#include <Storage/SATA.h>
#include <Util/Buffer.h>
#include <Storage/VFS.h>
#include <Task/Task.h>
#include <Memory/VMM.h>
#include <Memory/PMM.h>
#include <CPU/APIC.h>
#include <CPU/PCI.h>
#include <CPU/IO.h>
#include <CPU/ISR.h>
#include <Debug/KDebug.h>

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
static uintptr_t AHCI_base_phys = 0;
static uint8_t AHCI_irq_mode = AHCI_IRQ_MODE_POLL;
static uint64_t AHCI_irq_count = 0;
static bool AHCI_irq_vector_registered = false;

static uint32_t SATA_device_count = 0;
static HBA_PORT_t* BLOCK_DEVICES[AHCI_MAX_SLOT];

extern uintptr_t ROOT_DEV;

static void AHCI_irq_handler(interrupt_frame_t* frame);
static void AHCI_setup_interrupts(uint16_t bus, uint32_t slot, uint16_t function);
static void AHCI_init_wait_queues(void);

static task_wait_queue_t AHCI_port_waitq[AHCI_MAX_SLOT];
static uint32_t AHCI_port_irq_error[AHCI_MAX_SLOT];
static bool AHCI_waitq_ready = false;

static int AHCI_port_index(HBA_PORT_t* port)
{
    if (!AHCI_base_address || !port)
        return -1;

    uintptr_t ports_base = (uintptr_t) &AHCI_base_address->ports[0];
    uintptr_t port_addr = (uintptr_t) port;
    if (port_addr < ports_base)
        return -1;

    uintptr_t delta = port_addr - ports_base;
    if ((delta % sizeof(HBA_PORT_t)) != 0)
        return -1;

    uint32_t index = (uint32_t) (delta / sizeof(HBA_PORT_t));
    if (index >= AHCI_MAX_SLOT)
        return -1;

    return (int) index;
}

static bool AHCI_wait_slot_pending(void* context)
{
    AHCI_wait_slot_ctx_t* wait_ctx = (AHCI_wait_slot_ctx_t*) context;
    if (!wait_ctx || !wait_ctx->port)
        return false;

    if (wait_ctx->port_index < AHCI_MAX_SLOT &&
        __atomic_load_n(&AHCI_port_irq_error[wait_ctx->port_index], __ATOMIC_ACQUIRE) != 0)
    {
        return false;
    }

    return (wait_ctx->port->ci & wait_ctx->slot_mask) != 0;
}

static bool AHCI_wait_for_slot_poll(HBA_PORT_t* port, uint32_t slot_mask, uint32_t port_index)
{
    uint32_t spin = 0;
    while ((port->ci & slot_mask) && spin < SATA_IO_MAX_WAIT)
    {
        if ((port->is & HBA_PxIS_TFES) ||
            (port_index < AHCI_MAX_SLOT &&
             __atomic_load_n(&AHCI_port_irq_error[port_index], __ATOMIC_ACQUIRE) != 0))
        {
            return false;
        }

        spin++;
        __asm__ __volatile__("pause");
    }

    return spin != SATA_IO_MAX_WAIT;
}

static bool AHCI_wait_for_slot_irq(HBA_PORT_t* port, uint32_t slot_mask, uint32_t port_index)
{
    if (!AHCI_waitq_ready || port_index >= AHCI_MAX_SLOT || AHCI_get_irq_mode() == AHCI_IRQ_MODE_POLL)
        return false;

    AHCI_wait_slot_ctx_t wait_ctx = {
        .port = port,
        .slot_mask = slot_mask,
        .port_index = port_index
    };

    task_waiter_t waiter = { 0 };
    uint64_t timeout_ticks = task_ticks_from_ms(AHCI_IO_WAIT_TIMEOUT_MS);
    if (timeout_ticks == 0)
        timeout_ticks = 1;

    return task_wait_queue_wait_event(&AHCI_port_waitq[port_index],
                                      &waiter,
                                      AHCI_wait_slot_pending,
                                      &wait_ctx,
                                      timeout_ticks);
}

static bool AHCI_wait_for_slot_completion(HBA_PORT_t* port, uint32_t slot_mask, uint32_t port_index)
{
    if (AHCI_wait_for_slot_irq(port, slot_mask, port_index))
        return true;

    return AHCI_wait_for_slot_poll(port, slot_mask, port_index);
}

static void AHCI_init_wait_queues(void)
{
    if (AHCI_waitq_ready)
        return;

    for (uint32_t port = 0; port < AHCI_MAX_SLOT; port++)
    {
        task_wait_queue_init(&AHCI_port_waitq[port]);
        __atomic_store_n(&AHCI_port_irq_error[port], 0, __ATOMIC_RELAXED);
    }

    AHCI_waitq_ready = true;
}

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
        AHCI_base_phys = AHCI_base;
        AHCI_base_address = (HBA_MEM_t*) VMM_get_AHCI_virt();
        VMM_map_mmio_uc_pages((uintptr_t) AHCI_base_address, AHCI_base, AHCI_MEM_LENGTH);
        AHCI_init_wait_queues();
        AHCI_setup_interrupts(bus, slot, function);
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

uint8_t AHCI_get_irq_mode(void)
{
    return __atomic_load_n(&AHCI_irq_mode, __ATOMIC_ACQUIRE);
}

uint64_t AHCI_get_irq_count(void)
{
    return __atomic_load_n(&AHCI_irq_count, __ATOMIC_ACQUIRE);
}

const char* AHCI_get_irq_mode_name(void)
{
    switch (AHCI_get_irq_mode())
    {
        case AHCI_IRQ_MODE_MSIX:
            return "MSI-X";
        case AHCI_IRQ_MODE_MSI:
            return "MSI";
        default:
            return "INTx/POLL";
    }
}

static void AHCI_irq_handler(interrupt_frame_t* frame)
{
    (void) frame;

    if (!AHCI_base_address)
        goto out_eoi;

    uint32_t hba_is = AHCI_base_address->is;
    uint32_t pi = AHCI_base_address->pi;
    uint32_t active = hba_is & pi;
    uint32_t port_stuck_mask = 0;
    if (active != 0)
    {
        for (uint8_t port = 0; port < 32; port++)
        {
            uint32_t bit = 1U << port;
            if ((active & bit) == 0)
                continue;

            HBA_PORT_t* hba_port = (HBA_PORT_t*) &AHCI_base_address->ports[port];
            uint32_t port_is = hba_port->is;
            if (port_is != 0)
            {
                if ((port_is & HBA_PxIS_TFES) != 0 && port < AHCI_MAX_SLOT)
                    __atomic_store_n(&AHCI_port_irq_error[port], port_is, __ATOMIC_RELEASE);

                hba_port->is = port_is;
                if (hba_port->is != 0)
                    port_stuck_mask |= bit;
            }

            if (AHCI_waitq_ready && port < AHCI_MAX_SLOT)
                task_wait_queue_wake_all(&AHCI_port_waitq[port]);
        }
    }

    uint32_t hba_is_after = 0;
    if (hba_is != 0)
    {
        AHCI_base_address->is = hba_is;
        hba_is_after = AHCI_base_address->is;
        if (hba_is_after != 0)
        {
            // Retry once if status is still pending after first W1C.
            AHCI_base_address->is = hba_is_after;
            hba_is_after = AHCI_base_address->is;
        }
    }

    uint64_t irq_count = __atomic_add_fetch(&AHCI_irq_count, 1, __ATOMIC_RELAXED);
    if ((irq_count == 1 || (irq_count % 512ULL) == 0) &&
        (hba_is != 0 || hba_is_after != 0 || port_stuck_mask != 0))
    {
        kdebug_printf("[AHCI] irq mode=%s count=%llu hba_is=0x%X hba_is_after=0x%X px_stuck=0x%X\n",
                      AHCI_get_irq_mode_name(),
                      (unsigned long long) irq_count,
                      hba_is,
                      hba_is_after,
                      port_stuck_mask);
    }

out_eoi:
    if (APIC_is_enabled())
        APIC_send_EOI();
    task_irq_exit();
}

static void AHCI_setup_interrupts(uint16_t bus, uint32_t slot, uint16_t function)
{
    if (!AHCI_base_address || !APIC_is_enabled())
        return;

    AHCI_init_wait_queues();

    if (!AHCI_irq_vector_registered)
    {
        ISR_register_vector(AHCI_IRQ_VECTOR, AHCI_irq_handler);
        AHCI_irq_vector_registered = true;
    }

    uint32_t pi = AHCI_base_address->pi;
    AHCI_base_address->is = (uint32_t) -1;
    for (uint8_t port = 0; port < 32; port++)
    {
        if ((pi & (1U << port)) == 0)
            continue;
        AHCI_base_address->ports[port].is = (uint32_t) -1;
        if (port < AHCI_MAX_SLOT)
            __atomic_store_n(&AHCI_port_irq_error[port], 0, __ATOMIC_RELAXED);
    }

    uint8_t bsp_apic = APIC_get_bsp_lapic_id();
    bool enabled = false;
    if (PCI_enable_msix((uint8_t) bus,
                        (uint8_t) slot,
                        (uint8_t) function,
                        AHCI_base_phys,
                        (uintptr_t) AHCI_base_address,
                        AHCI_MEM_LENGTH,
                        AHCI_IRQ_VECTOR,
                        bsp_apic))
    {
        __atomic_store_n(&AHCI_irq_mode, AHCI_IRQ_MODE_MSIX, __ATOMIC_RELEASE);
        enabled = true;
    }
    else if (PCI_enable_msi((uint8_t) bus,
                            (uint8_t) slot,
                            (uint8_t) function,
                            AHCI_IRQ_VECTOR,
                            bsp_apic))
    {
        __atomic_store_n(&AHCI_irq_mode, AHCI_IRQ_MODE_MSI, __ATOMIC_RELEASE);
        enabled = true;
    }
    else
    {
        __atomic_store_n(&AHCI_irq_mode, AHCI_IRQ_MODE_POLL, __ATOMIC_RELEASE);
    }

    if (enabled)
    {
        uint16_t command = PCI_config_readw((uint8_t) bus, (uint8_t) slot, (uint8_t) function, PCI_COMMAND_REG);
        if ((command & PCI_COMMAND_INTX_DISABLE) == 0)
        {
            command |= PCI_COMMAND_INTX_DISABLE;
            PCI_config_writew((uint8_t) bus, (uint8_t) slot, (uint8_t) function, PCI_COMMAND_REG, command);
            command = PCI_config_readw((uint8_t) bus, (uint8_t) slot, (uint8_t) function, PCI_COMMAND_REG);
        }

        AHCI_base_address->ghc |= HBA_GHC_IE;
        kdebug_printf("[AHCI] irq configured mode=%s vec=0x%X b=%u s=%u f=%u pci_cmd=0x%X intx=%s\n",
                      AHCI_get_irq_mode_name(),
                      AHCI_IRQ_VECTOR,
                      (unsigned) bus,
                      (unsigned) slot,
                      (unsigned) function,
                      command,
                      (command & PCI_COMMAND_INTX_DISABLE) ? "disabled" : "enabled");
    }
    else
    {
        AHCI_base_address->ghc &= ~HBA_GHC_IE;
        kdebug_printf("[AHCI] irq fallback mode=%s b=%u s=%u f=%u\n",
                      AHCI_get_irq_mode_name(),
                      (unsigned) bus,
                      (unsigned) slot,
                      (unsigned) function);
    }
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

            bool whole_disk_ext4 = ext4_check_format(port);
            if (whole_disk_ext4)
                printf("\tWhole disk is ext4.\n");
            else
                printf("\tWhole disk is not ext4 (root may be on a partition).\n");
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

    uintptr_t clb_phys = (uintptr_t) PMM_alloc_page();
    uintptr_t fb_phys = (uintptr_t) PMM_alloc_page();
    if (clb_phys == 0 || fb_phys == 0)
        panic("SATA: failed to allocate CLB/FIS buffers !");

    void* clb_virt = (void*) P2V(clb_phys);
    void* fb_virt = (void*) P2V(fb_phys);

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
        uintptr_t ct_phys = (uintptr_t) PMM_alloc_page();
        if (ct_phys == 0)
            panic("SATA CTBA allocation failed !");
        void* ct_virt = (void*) P2V(ct_phys);

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

    if (count == 0)
        return SATA_IO_SUCCESS;

    int slot = AHCI_find_cmdslot(port);
    if (slot == -1)
        return SATA_IO_ERROR_NO_SLOT;

    uint32_t slot_mask = 1U << (uint32_t) slot;
    int port_index = AHCI_port_index(port);
    uint32_t port_index_u = (port_index >= 0) ? (uint32_t) port_index : AHCI_MAX_SLOT;
    if (port_index_u < AHCI_MAX_SLOT)
        __atomic_store_n(&AHCI_port_irq_error[port_index_u], 0, __ATOMIC_RELEASE);

    uintptr_t clb_phys = HILO2ADDR(port->clbu, port->clb);
    uintptr_t clb_virt = 0;
    if (!VMM_phys_to_virt(clb_phys, &clb_virt))
        panic("SATA CLB is not mapped !");
    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) clb_virt;

    cmd_header += slot;
    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 0; // Read from device.
    cmd_header->prdbc = 0;

    uintptr_t ctba_phys = HILO2ADDR(cmd_header->ctbau, cmd_header->ctba);
    uintptr_t ctba_virt = 0;
    if (!VMM_phys_to_virt(ctba_phys, &ctba_virt))
        panic("SATA CTBA is not mapped !");
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

    int spin = 0;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < SATA_IO_MAX_WAIT) // Wait until the port is no longer busy.
    {
        spin++;
        __asm__ __volatile__ ("pause");
    }

    if (spin == SATA_IO_MAX_WAIT)
        return SATA_IO_ERROR_HUNG_PORT;

    port->ci = slot_mask; // Issue command.
    if (!AHCI_wait_for_slot_completion(port, slot_mask, port_index_u))
        return SATA_IO_ERROR_HUNG_PORT;

    if ((port->is & HBA_PxIS_TFES) ||
        (port_index_u < AHCI_MAX_SLOT &&
         __atomic_load_n(&AHCI_port_irq_error[port_index_u], __ATOMIC_ACQUIRE) != 0))
    {
        return SATA_IO_ERROR_HUNG_PORT;
    }

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

    uint32_t slot_mask = 1U << (uint32_t) slot;
    int port_index = AHCI_port_index(port);
    uint32_t port_index_u = (port_index >= 0) ? (uint32_t) port_index : AHCI_MAX_SLOT;
    if (port_index_u < AHCI_MAX_SLOT)
        __atomic_store_n(&AHCI_port_irq_error[port_index_u], 0, __ATOMIC_RELEASE);

    uintptr_t clb_phys = HILO2ADDR(port->clbu, port->clb);
    uintptr_t clb_virt = 0;
    if (!VMM_phys_to_virt(clb_phys, &clb_virt))
        panic("SATA CLB is not mapped !");
    HBA_CMD_HEADER_t* cmd_header = (HBA_CMD_HEADER_t*) clb_virt;
    cmd_header += slot;

    cmd_header->cfl = sizeof (FIS_REG_H2D_t) / sizeof (uint32_t);
    cmd_header->w = 1; // Write to device.
    cmd_header->c = 1;
    cmd_header->prdbc = 0;

    uintptr_t ctba_phys = HILO2ADDR(cmd_header->ctbau, cmd_header->ctba);
    uintptr_t ctba_virt = 0;
    if (!VMM_phys_to_virt(ctba_phys, &ctba_virt))
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

    port->ci = slot_mask; // Issue command.
    if (!AHCI_wait_for_slot_completion(port, slot_mask, port_index_u))
        return SATA_IO_ERROR_HUNG_PORT;

    if ((port->is & HBA_PxIS_TFES) ||
        (port_index_u < AHCI_MAX_SLOT &&
         __atomic_load_n(&AHCI_port_irq_error[port_index_u], __ATOMIC_ACQUIRE) != 0))
    {
        return SATA_IO_ERROR_HUNG_PORT;
    }

    return SATA_IO_SUCCESS;
}
