#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define PCI_CONF_ADDR_IO_PORT   0xCF8
#define PCI_CONF_DATA_IO_PORT   0xCFC

#define PCI_DEV_CLASS_STORAGE   0x1
#define PCI_DEV_CLASS_BRIDGE    0x6

#define PCI_ID_REG          0x00
#define PCI_CLASS_REG       0x08
#define PCI_BHLC_REG        0x0C
#define PCI_INTERRUPT_REG   0x3C

#define PCI_CLASS(c)            (((c) >> 0x18) & 0xFF)
#define PCI_VENDOR(v)           (v & 0xFFFF)
#define PCI_INTERRUPT_LINE(l)   (l & 0xFF)

#define PCI_HDRTYPE(bhlcr)          (((bhlcr) >> 0x10) & 0xFF)
#define PCI_HDRTYPE_TYPE(bhlcr)     (PCI_HDRTYPE(bhlcr) & 0x7F)
#define PCI_HDRTYPE_MULTIFN(bhlcr)  ((PCI_HDRTYPE(bhlcr) & 0x80) != 0)

typedef struct PCI_bus PCI_bus_t;

typedef struct PCI_func
{
    PCI_bus_t* bus;
    uint32_t dev;
    uint32_t func;
    uint32_t dev_id;
    uint32_t dev_class;

    uint32_t reg_base[6];
    uint32_t reg_size[6];
    uint8_t IRQ_line;
} PCI_func_t;

typedef struct PCI_bus
{
    PCI_func_t* parent_bridge;
    uint32_t bus_no;
} PCI_bus_t;



void PCI_init(void);

static void PCI_conf_set_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset);
uint32_t PCI_config_read(PCI_func_t* func, uint8_t offset);

bool PCI_scan_device(PCI_func_t* func);
int PCI_scan_bus(PCI_bus_t* bus);
static void PCI_try_attach(PCI_func_t* func);
static void PCI_attach_storage_dev(PCI_func_t* func);

#endif