#ifndef _SATA_H
#define _SATA_H

#define SATA_IO_MAX_WAIT    10000

#define SATA_SIG_ATAPI      0xEB140101  // SATAPI drive.
#define SATA_SIG_SEMB       0xC33C0101  // Enclosure management bridge.
#define SATA_SIG_PM         0x96690101  // Port multiplier. 

/* SATA I/O status codes: */
#define SATA_IO_SUCCESS         0
#define SATA_IO_ERROR_NO_SLOT   3
#define SATA_IO_ERROR_HUNG_PORT 4

#endif