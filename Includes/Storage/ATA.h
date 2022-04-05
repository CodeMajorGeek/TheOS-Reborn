#ifndef _ATA_H
#define _ATA_H

#include <stdint.h>

typedef struct ATA_Device
{
    uint16_t base;
    uint16_t dev_ctrl;
} ATA_Device_t;

#define ATADEV_UNKNOWN  0
#define ATADEV_PATAPI   1
#define ATADEV_SATAPI   2
#define ATADEV_PATA     3
#define ATADEV_SATA     4

#define ATA_PRIMARY_IO_BUS      0x1F0
#define ATA_PRIMARY_CTRL_BUS    0x3F6

#define ATA_SECONDARY_IO_BUS    0x170
#define ATA_SECONDARY_CTRL_BUS  0x376

/* Offset from "I/O" base. */
#define ATA_REG_DATA            0
#define ATA_REG_ERR             1
#define ATA_REG_FEATURES        1
#define ATA_REG_SECTOR_COUNT    2
#define ATA_REG_SECTOR_NUM      3
#define ATA_REG_CYL_LO          4
#define ATA_REG_CYL_HI          5
#define ATA_REG_DEVSEL          6
#define ATA_REG_STATUS          7
#define ATA_REG_CMD             7

/* Offset from "Control" base. */
#define ATA_REG_ALT_STATUS      0
#define ATA_REG_DEV_CTRL        0
#define ATA_REG_DRIVE_ADDR      1

/* Error register. */
#define ATA_ERR_AMNF            1
#define ATA_ERR_TKZNF           1 << 1
#define ATA_ERR_ABRT            1 << 2
#define ATA_ERR_MCR             1 << 3
#define ATA_ERR_IDNF            1 << 4
#define ATA_ERR_MC              1 << 5
#define ATA_ERR_UNC             1 << 6
#define ATA_ERR_BBK             1 << 7

/* Drive / Head register (I/O base + 6). */
#define ATA_DRV                 1 << 4
#define ATA_LBA                 1 << 6

/* Status register (I/O base + 7). */
#define ATA_STATUS_ERR          1
#define ATA_STATUS_IDX          1 << 1
#define ATA_STATUS_CORR         1 << 2
#define ATA_STATUS_DRQ          1 << 3
#define ATA_STATUS_SRV          1 << 4
#define ATA_STATUS_DF           1 << 5
#define ATA_STATUS_RDY          1 << 6
#define ATA_STATUS_BSY          1 << 7

/* Device control regiter (control base + 0). */
#define ATA_DEV_INT             1 << 1
#define ATA_DEV_SRST            1 << 2
#define ATA_DEV_HOB             1 << 7

/* Drive address register (control base + 1). */
#define ATA_ADDR_DS0            1
#define ATA_ADDR_DS1            1 << 1
#define ATA_ADDR_HS0            1 << 2
#define ATA_ADDR_HS1            1 << 3
#define ATA_ADDR_HS2            1 << 4
#define ATA_ADDR_HS3            1 << 5
#define ATA_ADDR_WTG            1 << 6

void ATA_init(void);

void ATA_software_reset(uint16_t bus);

int ATA_detect_devtype(int slavebit, ATA_Device_t* ctrl);

#endif