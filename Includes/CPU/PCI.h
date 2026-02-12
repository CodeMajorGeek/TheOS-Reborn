#ifndef _PCI_H
#define _PCI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define PCI_CONF_ADDR_IO_PORT   0xCF8
#define PCI_CONF_DATA_IO_PORT   0xCFC

#define PCI_DEV_CLASS_STORAGE   0x01
#define PCI_DEV_CLASS_BRIDGE    0x06

#define PCI_VENDOR_REG          0x00
#define PCI_DEVICE_REG          0x02
#define PCI_COMMAND_REG         0x04
#define PCI_STATUS_REG          0x06
#define PCI_BIST_REG            0x0C
#define PCI_CLASS_REG           0x08
#define PCI_HEADER_TYPE_REG     0x0E
#define PCI_CAP_PTR_REG         0x34
#define PCI_BAR0_ADDR_REG       0x10
#define PCI_SECONDARY_BUS_REG   0x1A
#define PCI_BAR5_ADDR_REG       0x24

#define PCI_COMMAND_INTX_DISABLE    (1U << 10)
#define PCI_STATUS_CAP_LIST         (1U << 4)

#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11

typedef volatile struct PCI_msix_entry
{
    uint32_t msg_addr_lo;
    uint32_t msg_addr_hi;
    uint32_t msg_data;
    uint32_t vector_ctrl;
} PCI_msix_entry_t;

#define PCI_MSI_CTRL_MSI_ENABLE          (1U << 0)
#define PCI_MSI_CTRL_MULTI_MSG_EN_MASK   (7U << 4)
#define PCI_MSI_CTRL_64BIT_CAPABLE       (1U << 7)

#define PCI_MSIX_CTRL_ENABLE             (1U << 15)
#define PCI_MSIX_CTRL_FUNCTION_MASK      (1U << 14)
#define PCI_MSIX_CTRL_TABLE_SIZE_MASK    0x07FFU
#define PCI_MSIX_TABLE_BIR_MASK          0x7U
#define PCI_MSIX_TABLE_OFFSET_MASK       (~0x7U)
#define PCI_MSIX_VECTOR_MASK_BIT         (1U << 0)

#define PCI_MSI_ADDR_BASE                0xFEE00000U
#define PCI_MSIX_SCRATCH_VIRT_BASE       0xFFFFC00000300000ULL

typedef enum PCI_irq_mode
{
    PCI_IRQ_MODE_INTX = 0,
    PCI_IRQ_MODE_MSI = 1,
    PCI_IRQ_MODE_MSIX = 2
} PCI_irq_mode_t;

typedef struct PCI_bus PCI_bus_t;

void PCI_init(void);

uint8_t PCI_config_readb(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t PCI_config_read(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t PCI_config_readw(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void PCI_config_write(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
void PCI_config_writew(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void PCI_config_writeb(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint8_t value);

uint8_t PCI_find_capability(uint8_t bus, uint8_t slot, uint8_t function, uint8_t cap_id);
bool PCI_enable_msi(uint8_t bus, uint8_t slot, uint8_t function, uint8_t vector, uint8_t dest_apic_id);
bool PCI_enable_msix(uint8_t bus, uint8_t slot, uint8_t function, uintptr_t mmio_bar5_phys, uintptr_t mmio_bar5_virt, size_t mmio_bar5_len, uint8_t vector, uint8_t dest_apic_id);

void PCI_check_device(uint8_t bus, uint8_t slot);
void PCI_check_function(uint8_t bus, uint8_t slot, uint8_t function);

void PCI_scan_bus(uint8_t bus);

static void PCI_try_attach(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_storage_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);

#endif
