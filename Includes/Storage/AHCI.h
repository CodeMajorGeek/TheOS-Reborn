#ifndef _AHCI_H
#define _AHCI_H

#include <stdint.h>
#include <stdbool.h>

#define AHCI_MEM_LENGTH     0x1100

#define AHCI_MAX_SLOT       0x1F

#define FIS_TYPE_REG_H2D        0x27
#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35

#define ATA_DEV_BUSY            0x80
#define ATA_DEV_DRQ             0x08

#define FIS_LBA_MODE            (1 << 6)

/* AHCI vendors: */
#define AHCI_VENDOR_INTEL   0x8086
#define AHCI_VENDOR_VMWARE  0x15AD

/* AHCI devices: */
#define AHCI_ICH7_SATA      0x27C0

#define HBA_PORT_IPM_ACTIVE     0x1
#define HBA_PORT_DET_PRESENT    0x3

#define HBA_PxCMD_ST            0x0001
#define HBA_PxCMD_FRE           0x0010
#define HBA_PxCMD_FR            0x4000
#define HBA_PxCMD_CR            0x8000

typedef struct AHCI_device
{
    uint16_t vendor;
    uint16_t device;
    const char* name;
} AHCI_device_t;

typedef volatile struct HBA_PORT
{
    uint32_t clb;       // Command list base address, 1Kb aligned.
    uint32_t clbu;      // Command list base address upper 32 bits.
    uint32_t fb;        // FIS base address, 256 byte aligned.
    uint32_t fbu;       // FIS base address upper 32 bits.
    uint32_t is;        // Interrupt status.
    uint32_t ie;        // Interrupt enable.
    uint32_t cmd;       // Command and status.
    uint32_t rsv0;      // Reserved.
    uint32_t tfd;       // Task file data.
    uint32_t sig;       // Signature.
    uint32_t ssts;      // SATA status (SCR0:SStatus).
    uint32_t sctl;      // SATA control (SCR2:SControl).
    uint32_t serr;      // SATA error (SCR1:SError).
    uint32_t sact;      // SATA active (SCR3:SActive).
    uint32_t ci;        // Command issue.
    uint32_t sntf;      // SATA notification (SCR4:SNotification);
    uint32_t fbs;       // FIS-based switch control.
    uint32_t rsv1[11];  // Reserved.
    uint32_t vendor[4]; // Vendor specific.
} HBA_PORT_t;

typedef volatile struct HBA_MEM
{
    uint32_t cap;       // Host capability.
    uint32_t ghc;       // Global host control.
    uint32_t is;        // Interrupt status.
    uint32_t pi;        // Port implemented.
    uint32_t vs;        // Version.
    uint32_t ccc_ctl;   // Command completion coalescing control.
    uint32_t ccc_pts;   // Command completion coalescing ports.
    uint32_t em_loc;    // Enclosure management location.
    uint32_t em_ctl;    // Host capabilities control.
    uint32_t cap2;      // Host capabilities extended.
    uint32_t bohc;      // BIOS/OS handoff control and status.

    uint8_t rsv[0xA0 - 0x2C]; // Reserved.
    uint8_t vendor[0x100 - 0xA0]; // Vendor specific registers.
    HBA_PORT_t ports[1]; // Port control registers.
} HBA_MEM_t;

typedef struct HBA_CMD_HEADER
{
    // DW0:
    uint8_t cfl:5;              // Command FIS length in DWORDS, 2 ~ 16.
    uint8_t a:1;                // ATAPI.
    uint8_t w:1;                // Write, 1: H2D, 0: D2H.
    uint8_t p:1;                // Prefetchable.

    uint8_t r:1;                // Reset.
    uint8_t b:1;                // BIST.
    uint8_t c:1;                // Clear busy upon R_OK.
    uint8_t rsv0:1;             // Reserved.

    uint16_t prdtl;             // Physical region descriptor table length in entries.
    // DW1:
    volatile uint32_t prdbc;    // Physical region descriptor byte count transferred.
    // DW2, 3:
    uint32_t ctba;              // Command table descriptor base address.
    uint32_t ctbau;             // Command table descriptor base address upper 32 bits.
    // DW4 - 7:
    uint32_t rsc1[4];           // Reserved.

} HBA_CMD_HEADER_t;

typedef struct FIS_REG_H2D
{
    // DW0:
    uint8_t fis_type;   // FIS_TYPE_REG_H2D.

    uint8_t pmport:4;   // Port multiplier.
    uint8_t rsv0:3;     // Reserved.
    uint8_t c:1;        // 1: Command, 0: Control.

    uint8_t command;    // Command register.
    uint8_t featurel;   // Feature register, 7:0.
    // DW1:
    uint8_t lba0;       // LBA low register, 7:0.
    uint8_t lba1;       // LBA mid register, 15:8.
    uint8_t lba2;       // LBA high register, 23:16;
    uint8_t device;     // Device register.
    // DW2:
    uint8_t lba3;       // LBA register, 31:24.
    uint8_t lba4;       // LBA register, 39:32.
    uint8_t lba5;       // LBA register, 47:40.
    uint8_t featureh;   // Feature register, 15:8.
    // DW3:
    uint8_t countl;     // Count register, 7:0.
    uint8_t counth;     // Count register, 15:8.
    uint8_t icc;        // Isochronous command completion.
    uint8_t control;    // Control register.
    // DW4:
    uint8_t rsv1[4];    // Reserved.
} FIS_REG_H2D_t;

typedef struct HBA_PRDT_ENTRY
{
    uint32_t dba;       // Data base address.
    uint32_t dbau;      // Data base address upper 32 bits.
    uint32_t rsv0;      // Reserved.
    // DW3:
    uint32_t dbc:22;    // Byte count, 4M max.
    uint32_t rsv1:9;    // Reserved.
    uint32_t i:1;       // Interrupt on completion.
} HBA_PRDT_ENTRY_t;


typedef struct HBA_CMD_TBL
{
    uint8_t cfis[64];               // Command FIS.
    uint8_t acmd[16];               // ATAPI command, 12 or 16 bytes.
    uint8_t rsv[48];                // Reserved.
    HBA_PRDT_ENTRY_t prdt_entry[1]; // Physical region descriptor table entries, 0 ~ 65535.
} HBA_CMD_TBL_t;

void AHCI_set_port_base(uintptr_t port_base);

void AHCI_try_setup_device(uint16_t bus, uint32_t slot, uint16_t function, uint16_t vendor, uint16_t device);
void AHCI_try_setup_known_device(char* name, uintptr_t AHCI_base, uint16_t bus, uint16_t slot, uint16_t func);

void AHCI_SATA_init(HBA_PORT_t* port, int num);
bool AHCI_rebase_port(HBA_PORT_t* port, int num);

bool AHCI_stop_port(HBA_PORT_t* port);
void AHCI_start_port(HBA_PORT_t* port);

int AHCI_find_cmdslot(HBA_PORT_t* port);
int AHCI_sata_read(HBA_PORT_t* port, uint32_t startl, uint32_t starth, uint32_t count, uint8_t* buf);

#endif