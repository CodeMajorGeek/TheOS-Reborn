#include <Device/E1000.h>
#include <Device/E1000_private.h>

#include <CPU/APIC.h>
#include <CPU/PCI.h>
#include <Debug/KDebug.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Network/ARP.h>
#include <Network/Socket.h>
#include <Network/TCP.h>

#include <string.h>

static e1000_runtime_state_t E1000_state = {
    .irq_mode = E1000_IRQ_MODE_POLL
};

static void E1000_cpu_relax(void)
{
    __asm__ __volatile__("pause");
}

static const char* E1000_irq_mode_name_from_value(uint8_t mode)
{
    switch (mode)
    {
        case E1000_IRQ_MODE_MSIX:
            return "MSI-X";
        case E1000_IRQ_MODE_MSI:
            return "MSI";
        default:
            return "INTx/POLL";
    }
}

static inline uint32_t E1000_mmio_read32(uint32_t reg)
{
    volatile uint32_t* ptr = (volatile uint32_t*) (E1000_state.mmio_virt + (uintptr_t) reg);
    return *ptr;
}

static inline void E1000_mmio_write32(uint32_t reg, uint32_t value)
{
    volatile uint32_t* ptr = (volatile uint32_t*) (E1000_state.mmio_virt + (uintptr_t) reg);
    *ptr = value;
}

static inline uint16_t E1000_ring_next(uint16_t index, uint16_t count)
{
    uint16_t next = (uint16_t) (index + 1U);
    if (next >= count)
        next = 0;
    return next;
}

static bool E1000_is_supported_device(uint16_t vendor, uint16_t device)
{
    if (vendor != E1000_VENDOR_INTEL)
        return false;

    switch (device)
    {
        case E1000_DEVICE_82574L:
            return true;
        default:
            return false;
    }
}

static bool E1000_wait_ctrl_reset_clear(uint32_t timeout_loops)
{
    for (uint32_t i = 0; i < timeout_loops; i++)
    {
        if ((E1000_mmio_read32(E1000_REG_CTRL) & E1000_CTRL_RST) == 0)
            return true;
        E1000_cpu_relax();
    }

    return false;
}

static void E1000_read_mac_locked(void)
{
    uint32_t ral0 = E1000_mmio_read32(E1000_REG_RAL0);
    uint32_t rah0 = E1000_mmio_read32(E1000_REG_RAH0);

    if ((rah0 & E1000_RAH_ADDR_VALID) == 0)
    {
        memset(E1000_state.mac, 0, sizeof(E1000_state.mac));
        return;
    }

    E1000_state.mac[0] = (uint8_t) ((ral0 >> 0) & 0xFFU);
    E1000_state.mac[1] = (uint8_t) ((ral0 >> 8) & 0xFFU);
    E1000_state.mac[2] = (uint8_t) ((ral0 >> 16) & 0xFFU);
    E1000_state.mac[3] = (uint8_t) ((ral0 >> 24) & 0xFFU);
    E1000_state.mac[4] = (uint8_t) ((rah0 >> 0) & 0xFFU);
    E1000_state.mac[5] = (uint8_t) ((rah0 >> 8) & 0xFFU);
}

static bool E1000_alloc_rings_locked(void)
{
    if (!E1000_state.rx_desc_virt)
    {
        E1000_state.rx_desc_phys = (uintptr_t) PMM_alloc_page();
        if (E1000_state.rx_desc_phys == 0)
            return false;
        E1000_state.rx_desc_virt = (e1000_rx_desc_t*) P2V(E1000_state.rx_desc_phys);
        memset(E1000_state.rx_desc_virt, 0, PHYS_PAGE_SIZE);
    }

    if (!E1000_state.tx_desc_virt)
    {
        E1000_state.tx_desc_phys = (uintptr_t) PMM_alloc_page();
        if (E1000_state.tx_desc_phys == 0)
            return false;
        E1000_state.tx_desc_virt = (e1000_tx_desc_t*) P2V(E1000_state.tx_desc_phys);
        memset(E1000_state.tx_desc_virt, 0, PHYS_PAGE_SIZE);
    }

    for (uint16_t i = 0; i < E1000_RX_DESC_COUNT; i++)
    {
        if (!E1000_state.rx_buf_virt[i])
        {
            E1000_state.rx_buf_phys[i] = (uintptr_t) PMM_alloc_page();
            if (E1000_state.rx_buf_phys[i] == 0)
                return false;

            E1000_state.rx_buf_virt[i] = (uint8_t*) P2V(E1000_state.rx_buf_phys[i]);
        }

        memset(E1000_state.rx_buf_virt[i], 0, E1000_RX_BUFFER_BYTES);
        E1000_state.rx_desc_virt[i].addr = (uint64_t) E1000_state.rx_buf_phys[i];
        E1000_state.rx_desc_virt[i].length = 0;
        E1000_state.rx_desc_virt[i].checksum = 0;
        E1000_state.rx_desc_virt[i].status = 0;
        E1000_state.rx_desc_virt[i].errors = 0;
        E1000_state.rx_desc_virt[i].special = 0;
    }

    for (uint16_t i = 0; i < E1000_TX_DESC_COUNT; i++)
    {
        if (!E1000_state.tx_buf_virt[i])
        {
            E1000_state.tx_buf_phys[i] = (uintptr_t) PMM_alloc_page();
            if (E1000_state.tx_buf_phys[i] == 0)
                return false;

            E1000_state.tx_buf_virt[i] = (uint8_t*) P2V(E1000_state.tx_buf_phys[i]);
        }

        memset(E1000_state.tx_buf_virt[i], 0, E1000_TX_BUFFER_BYTES);
        E1000_state.tx_desc_virt[i].addr = (uint64_t) E1000_state.tx_buf_phys[i];
        E1000_state.tx_desc_virt[i].length = 0;
        E1000_state.tx_desc_virt[i].cso = 0;
        E1000_state.tx_desc_virt[i].cmd = 0;
        E1000_state.tx_desc_virt[i].status = E1000_TX_DESC_STATUS_DD;
        E1000_state.tx_desc_virt[i].css = 0;
        E1000_state.tx_desc_virt[i].special = 0;
    }

    return true;
}

static void E1000_program_hw_rings_locked(void)
{
    E1000_state.rx_next_clean = 0;
    E1000_state.tx_next_use = 0;
    E1000_state.rx_sw_head = 0;
    E1000_state.rx_sw_tail = 0;
    __atomic_store_n(&E1000_state.rx_sw_count, 0U, __ATOMIC_RELEASE);

    E1000_mmio_write32(E1000_REG_RDBAL, ADDRLO(E1000_state.rx_desc_phys));
    E1000_mmio_write32(E1000_REG_RDBAH, ADDRHI(E1000_state.rx_desc_phys));
    E1000_mmio_write32(E1000_REG_RDLEN, (uint32_t) (E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t)));
    E1000_mmio_write32(E1000_REG_RDH, 0);
    E1000_mmio_write32(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1U);

    E1000_mmio_write32(E1000_REG_TDBAL, ADDRLO(E1000_state.tx_desc_phys));
    E1000_mmio_write32(E1000_REG_TDBAH, ADDRHI(E1000_state.tx_desc_phys));
    E1000_mmio_write32(E1000_REG_TDLEN, (uint32_t) (E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t)));
    E1000_mmio_write32(E1000_REG_TDH, 0);
    E1000_mmio_write32(E1000_REG_TDT, 0);
}

static void E1000_configure_rx_tx_locked(void)
{
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;
    E1000_mmio_write32(E1000_REG_RCTL, rctl);

    uint32_t tctl = E1000_TCTL_EN |
                    E1000_TCTL_PSP |
                    (E1000_TCTL_CT_DEFAULT << E1000_TCTL_CT_SHIFT) |
                    (E1000_TCTL_COLD_DEFAULT << E1000_TCTL_COLD_SHIFT);
    E1000_mmio_write32(E1000_REG_TCTL, tctl);
    E1000_mmio_write32(E1000_REG_TIPG, E1000_TIPG_DEFAULT);
}

static bool E1000_setup_interrupts_locked(uint8_t bus, uint8_t slot, uint8_t function)
{
    E1000_state.irq_count = 0;
    E1000_mmio_write32(E1000_REG_IMC, 0xFFFFFFFFU);
    (void) E1000_mmio_read32(E1000_REG_ICR);

    if (!APIC_is_enabled())
    {
        E1000_state.irq_mode = E1000_IRQ_MODE_POLL;
        kdebug_printf("[E1000] irq fallback mode=%s (APIC disabled)\n",
                      E1000_irq_mode_name_from_value(E1000_state.irq_mode));
        return false;
    }

    if (!E1000_state.irq_vector_registered)
    {
        ISR_register_vector(E1000_IRQ_VECTOR, E1000_irq_handler);
        E1000_state.irq_vector_registered = true;
    }

    uint8_t bsp_apic = APIC_get_bsp_lapic_id();
    bool enabled = false;

    if (PCI_enable_msix(bus, slot, function, 0, 0, 0, E1000_IRQ_VECTOR, bsp_apic))
    {
        E1000_state.irq_mode = E1000_IRQ_MODE_MSIX;
        enabled = true;
    }
    else if (PCI_enable_msi(bus, slot, function, E1000_IRQ_VECTOR, bsp_apic))
    {
        E1000_state.irq_mode = E1000_IRQ_MODE_MSI;
        enabled = true;
    }
    else
    {
        E1000_state.irq_mode = E1000_IRQ_MODE_POLL;
    }

    uint16_t command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
    if (enabled)
    {
        if ((command & PCI_COMMAND_INTX_DISABLE) == 0)
        {
            command |= PCI_COMMAND_INTX_DISABLE;
            PCI_config_writew(bus, slot, function, PCI_COMMAND_REG, command);
            command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
        }

        E1000_mmio_write32(E1000_REG_IMS, E1000_IRQ_MASK);
    }
    else
    {
        command &= (uint16_t) ~PCI_COMMAND_INTX_DISABLE;
        PCI_config_writew(bus, slot, function, PCI_COMMAND_REG, command);
        command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
    }

    kdebug_printf("[E1000] irq configured mode=%s vec=0x%X b=%u s=%u f=%u pci_cmd=0x%X intx=%s\n",
                  E1000_irq_mode_name_from_value(E1000_state.irq_mode),
                  (unsigned) E1000_IRQ_VECTOR,
                  (unsigned) bus,
                  (unsigned) slot,
                  (unsigned) function,
                  (unsigned) command,
                  (command & PCI_COMMAND_INTX_DISABLE) ? "disabled" : "enabled");

    return enabled;
}

static uint32_t E1000_poll_rx_locked(void)
{
    uint32_t processed = 0;

    while (processed < E1000_RX_DESC_COUNT)
    {
        e1000_rx_desc_t* desc = &E1000_state.rx_desc_virt[E1000_state.rx_next_clean];
        uint8_t status = desc->status;
        if ((status & E1000_RX_DESC_STATUS_DD) == 0)
            break;

        uint16_t frame_len = desc->length;
        if ((status & E1000_RX_DESC_STATUS_EOP) != 0 &&
            frame_len != 0 &&
            frame_len <= E1000_RX_BUFFER_BYTES)
        {
            ARP_note_ethernet_frame(E1000_state.rx_buf_virt[E1000_state.rx_next_clean], frame_len);
            NET_socket_on_ethernet_frame(E1000_state.rx_buf_virt[E1000_state.rx_next_clean], frame_len);
            NET_tcp_on_ethernet_frame(E1000_state.rx_buf_virt[E1000_state.rx_next_clean], frame_len);
            uint32_t queue_count = __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE);
            if (queue_count < E1000_RX_SW_QUEUE_LEN)
            {
                e1000_frame_slot_t* slot = &E1000_state.rx_sw_queue[E1000_state.rx_sw_tail];
                memcpy(slot->data, E1000_state.rx_buf_virt[E1000_state.rx_next_clean], frame_len);
                slot->len = frame_len;
                E1000_state.rx_sw_tail = E1000_ring_next(E1000_state.rx_sw_tail, E1000_RX_SW_QUEUE_LEN);
                __atomic_store_n(&E1000_state.rx_sw_count, queue_count + 1U, __ATOMIC_RELEASE);
                E1000_state.rx_packets++;
            }
            else
            {
                E1000_state.rx_dropped++;
            }
        }
        else
        {
            E1000_state.rx_dropped++;
        }

        desc->length = 0;
        desc->checksum = 0;
        desc->status = 0;
        desc->errors = 0;
        desc->special = 0;

        uint16_t replenished = E1000_state.rx_next_clean;
        E1000_state.rx_next_clean = E1000_ring_next(E1000_state.rx_next_clean, E1000_RX_DESC_COUNT);
        E1000_mmio_write32(E1000_REG_RDT, replenished);

        processed++;
    }

    if (processed != 0 &&
        E1000_state.waitq_ready &&
        __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE) != 0)
    {
        task_wait_queue_wake_all(&E1000_state.rx_waitq);
    }

    return processed;
}

static bool E1000_wait_rx_queue_empty_predicate(void* context)
{
    (void) context;

    if (!__atomic_load_n(&E1000_state.available, __ATOMIC_ACQUIRE))
        return false;

    return __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE) == 0U;
}

static void E1000_irq_handler(interrupt_frame_t* frame)
{
    (void) frame;

    if (!E1000_state.lock_ready)
        goto out_eoi;

    spin_lock(&E1000_state.lock);
    if (!E1000_state.available || E1000_state.mmio_virt == 0)
    {
        spin_unlock(&E1000_state.lock);
        goto out_eoi;
    }

    uint32_t icr = E1000_mmio_read32(E1000_REG_ICR);
    if (icr != 0)
    {
        E1000_state.irq_count++;

        if ((icr & E1000_ICR_LSC) != 0)
            E1000_state.link_up = (E1000_mmio_read32(E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;

        if ((icr & (E1000_ICR_RXDMT0 | E1000_ICR_RXT0 | E1000_ICR_RXO)) != 0)
            (void) E1000_poll_rx_locked();
    }

    spin_unlock(&E1000_state.lock);

out_eoi:
    if (APIC_is_enabled())
        APIC_send_EOI();
    task_irq_exit();
}

static bool E1000_controller_init_locked(uint8_t bus,
                                         uint8_t slot,
                                         uint8_t function,
                                         uint16_t vendor,
                                         uint16_t device)
{
    if (!E1000_is_supported_device(vendor, device))
        return false;

    uint32_t bar0_lo = PCI_config_read(bus, slot, function, PCI_BAR0_ADDR_REG);
    if (bar0_lo == 0 || bar0_lo == 0xFFFFFFFFU)
        return false;
    if ((bar0_lo & 0x1U) != 0)
        return false;

    uintptr_t mmio_phys = (uintptr_t) (bar0_lo & ~0xFULL);
    uint32_t bar0_type = (bar0_lo >> 1U) & 0x3U;
    if (bar0_type == 0x2U)
    {
        uint32_t bar1_hi = PCI_config_read(bus, slot, function, (uint8_t) (PCI_BAR0_ADDR_REG + 4U));
        mmio_phys = (((uintptr_t) bar1_hi) << 32) | mmio_phys;
    }
    if (mmio_phys == 0)
        return false;

    uintptr_t mmio_virt = VMM_MMIO_VIRT(mmio_phys);
    VMM_map_mmio_uc_pages(mmio_virt, mmio_phys, E1000_MMIO_WINDOW_SIZE);

    uint16_t pci_command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
    pci_command |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    PCI_config_writew(bus, slot, function, PCI_COMMAND_REG, pci_command);

    E1000_state.mmio_phys = mmio_phys;
    E1000_state.mmio_virt = mmio_virt;
    E1000_state.pci_bus = bus;
    E1000_state.pci_slot = slot;
    E1000_state.pci_function = function;
    E1000_state.vendor_id = vendor;
    E1000_state.device_id = device;

    uint32_t ctrl = E1000_mmio_read32(E1000_REG_CTRL);
    E1000_mmio_write32(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    if (!E1000_wait_ctrl_reset_clear(E1000_RESET_TIMEOUT_LOOPS))
        return false;

    E1000_read_mac_locked();
    if (!E1000_alloc_rings_locked())
        return false;

    E1000_program_hw_rings_locked();
    E1000_configure_rx_tx_locked();
    (void) E1000_setup_interrupts_locked(bus, slot, function);

    E1000_state.link_up = (E1000_mmio_read32(E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;
    E1000_state.controller_ready = true;
    E1000_state.available = true;

    kdebug_printf("[E1000] initialized b=%u s=%u f=%u vid=0x%X did=0x%X irq=%s link=%s mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  (unsigned) bus,
                  (unsigned) slot,
                  (unsigned) function,
                  (unsigned) vendor,
                  (unsigned) device,
                  E1000_irq_mode_name_from_value(E1000_state.irq_mode),
                  E1000_state.link_up ? "up" : "down",
                  (unsigned) E1000_state.mac[0],
                  (unsigned) E1000_state.mac[1],
                  (unsigned) E1000_state.mac[2],
                  (unsigned) E1000_state.mac[3],
                  (unsigned) E1000_state.mac[4],
                  (unsigned) E1000_state.mac[5]);

    return true;
}

void E1000_try_setup_device(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device)
{
    if (!E1000_state.lock_ready)
    {
        spinlock_init(&E1000_state.lock);
        E1000_state.lock_ready = true;
    }
    if (!E1000_state.waitq_ready)
    {
        task_wait_queue_init(&E1000_state.rx_waitq);
        E1000_state.waitq_ready = true;
    }

    spin_lock(&E1000_state.lock);
    if (!E1000_state.available)
    {
        if (!E1000_controller_init_locked(bus, slot, function, vendor, device))
        {
            E1000_state.available = false;
            E1000_state.controller_ready = false;
            E1000_state.irq_mode = E1000_IRQ_MODE_POLL;
        }
    }
    spin_unlock(&E1000_state.lock);
}

bool E1000_is_available(void)
{
    bool available = false;

    if (!E1000_state.lock_ready)
        return false;

    spin_lock(&E1000_state.lock);
    available = E1000_state.available;
    spin_unlock(&E1000_state.lock);
    return available;
}

bool E1000_get_mac(uint8_t out_mac[6])
{
    if (!out_mac || !E1000_state.lock_ready)
        return false;

    spin_lock(&E1000_state.lock);
    if (!E1000_state.available)
    {
        spin_unlock(&E1000_state.lock);
        return false;
    }

    memcpy(out_mac, E1000_state.mac, 6);
    spin_unlock(&E1000_state.lock);
    return true;
}

bool E1000_link_is_up(void)
{
    bool link_up = false;

    if (!E1000_state.lock_ready)
        return false;

    spin_lock(&E1000_state.lock);
    if (E1000_state.available)
    {
        E1000_state.link_up = (E1000_mmio_read32(E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;
        link_up = E1000_state.link_up;
    }
    spin_unlock(&E1000_state.lock);
    return link_up;
}

uint8_t E1000_get_irq_mode(void)
{
    uint8_t mode = E1000_IRQ_MODE_POLL;

    if (!E1000_state.lock_ready)
        return mode;

    spin_lock(&E1000_state.lock);
    mode = E1000_state.irq_mode;
    spin_unlock(&E1000_state.lock);
    return mode;
}

uint64_t E1000_get_irq_count(void)
{
    uint64_t count = 0;

    if (!E1000_state.lock_ready)
        return 0;

    spin_lock(&E1000_state.lock);
    count = E1000_state.irq_count;
    spin_unlock(&E1000_state.lock);
    return count;
}

const char* E1000_get_irq_mode_name(void)
{
    return E1000_irq_mode_name_from_value(E1000_get_irq_mode());
}

bool E1000_get_stats(sys_net_raw_stats_t* out_stats)
{
    if (!out_stats || !E1000_state.lock_ready)
        return false;

    memset(out_stats, 0, sizeof(*out_stats));

    spin_lock(&E1000_state.lock);
    if (!E1000_state.available)
    {
        spin_unlock(&E1000_state.lock);
        return false;
    }

    E1000_state.link_up = (E1000_mmio_read32(E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;
    out_stats->rx_packets = E1000_state.rx_packets;
    out_stats->rx_dropped = E1000_state.rx_dropped;
    out_stats->tx_packets = E1000_state.tx_packets;
    out_stats->tx_dropped = E1000_state.tx_dropped;
    out_stats->irq_count = E1000_state.irq_count;
    out_stats->rx_queue_depth = __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE);
    out_stats->rx_queue_capacity = E1000_RX_SW_QUEUE_LEN;
    out_stats->link_up = E1000_state.link_up ? 1U : 0U;
    out_stats->irq_mode = E1000_state.irq_mode;
    memcpy(out_stats->mac, E1000_state.mac, sizeof(out_stats->mac));
    spin_unlock(&E1000_state.lock);
    return true;
}

bool E1000_get_pending_rx_bytes(size_t* out_bytes)
{
    if (!out_bytes || !E1000_state.lock_ready)
        return false;

    *out_bytes = 0;
    spin_lock(&E1000_state.lock);
    if (!E1000_state.available)
    {
        spin_unlock(&E1000_state.lock);
        return false;
    }

    (void) E1000_poll_rx_locked();
    uint32_t queue_count = __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE);
    if (queue_count != 0U)
    {
        const e1000_frame_slot_t* slot = &E1000_state.rx_sw_queue[E1000_state.rx_sw_head];
        *out_bytes = slot->len;
    }

    spin_unlock(&E1000_state.lock);
    return true;
}

bool E1000_raw_read(uint8_t* out_frame, size_t out_cap, size_t* out_len, bool block)
{
    if (!out_frame || out_cap == 0 || !out_len)
        return false;

    *out_len = 0;
    if (!E1000_state.lock_ready)
        return false;

    for (;;)
    {
        if (block &&
            E1000_state.waitq_ready &&
            E1000_get_irq_mode() != E1000_IRQ_MODE_POLL)
        {
            task_waiter_t waiter = { 0 };
            if (!task_wait_queue_wait_event(&E1000_state.rx_waitq,
                                            &waiter,
                                            E1000_wait_rx_queue_empty_predicate,
                                            NULL,
                                            TASK_WAIT_TIMEOUT_INFINITE))
            {
                return false;
            }
        }

        spin_lock(&E1000_state.lock);
        if (!E1000_state.available)
        {
            spin_unlock(&E1000_state.lock);
            return false;
        }

        (void) E1000_poll_rx_locked();

        uint32_t queue_count = __atomic_load_n(&E1000_state.rx_sw_count, __ATOMIC_ACQUIRE);
        if (queue_count == 0U)
        {
            spin_unlock(&E1000_state.lock);
            if (!block || E1000_state.irq_mode == E1000_IRQ_MODE_POLL)
                return true;
            continue;
        }

        e1000_frame_slot_t* slot = &E1000_state.rx_sw_queue[E1000_state.rx_sw_head];
        size_t frame_len = slot->len;
        size_t copy_len = frame_len;
        if (copy_len > out_cap)
            copy_len = out_cap;

        memcpy(out_frame, slot->data, copy_len);
        E1000_state.rx_sw_head = E1000_ring_next(E1000_state.rx_sw_head, E1000_RX_SW_QUEUE_LEN);
        __atomic_store_n(&E1000_state.rx_sw_count, queue_count - 1U, __ATOMIC_RELEASE);
        spin_unlock(&E1000_state.lock);

        *out_len = copy_len;
        return true;
    }
}

bool E1000_raw_write(const uint8_t* frame, size_t len, size_t* out_written)
{
    if (!frame || !out_written)
        return false;

    *out_written = 0;
    if (len < E1000_ETH_FRAME_MIN_BYTES ||
        len > E1000_ETH_FRAME_MAX_BYTES ||
        len > E1000_TX_BUFFER_BYTES ||
        !E1000_state.lock_ready)
    {
        return false;
    }

    spin_lock(&E1000_state.lock);
    if (!E1000_state.available || !E1000_state.tx_desc_virt)
    {
        spin_unlock(&E1000_state.lock);
        return false;
    }

    uint16_t tx_index = E1000_state.tx_next_use;
    e1000_tx_desc_t* tx_desc = &E1000_state.tx_desc_virt[tx_index];

    uint32_t spin = 0;
    while ((tx_desc->status & E1000_TX_DESC_STATUS_DD) == 0 &&
           spin < E1000_TX_WAIT_TIMEOUT_LOOPS)
    {
        spin++;
        E1000_cpu_relax();
    }

    if ((tx_desc->status & E1000_TX_DESC_STATUS_DD) == 0)
    {
        E1000_state.tx_dropped++;
        spin_unlock(&E1000_state.lock);
        return false;
    }

    memcpy(E1000_state.tx_buf_virt[tx_index], frame, len);
    tx_desc->length = (uint16_t) len;
    tx_desc->cso = 0;
    tx_desc->cmd = E1000_TX_DESC_CMD_EOP | E1000_TX_DESC_CMD_IFCS | E1000_TX_DESC_CMD_RS;
    tx_desc->status = 0;
    tx_desc->css = 0;
    tx_desc->special = 0;

    E1000_state.tx_next_use = E1000_ring_next(tx_index, E1000_TX_DESC_COUNT);
    E1000_mmio_write32(E1000_REG_TDT, E1000_state.tx_next_use);
    E1000_state.tx_packets++;

    spin_unlock(&E1000_state.lock);
    *out_written = len;
    return true;
}
