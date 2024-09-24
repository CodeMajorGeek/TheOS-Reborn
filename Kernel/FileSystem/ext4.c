#include <FileSystem/ext4.h>

#include <stdio.h>

bool ext4_check_format(HBA_PORT_t* port)
{
    char buf[512];
    AHCI_sata_read(port, EXT4_SUPERBLOCK_ADDR / AHCI_SECTOR_SIZE, 0, 1, buf);

    // for (int i = 0; i < sizeof(buf); i++)
    //     printf("0x%2X ", buf[i]);
    // puts("");

    ext4_superblock_t* superblock = (ext4_superblock_t*) buf;
    return superblock->magic == EXT4_MAGIC_SIGNATURE;
}