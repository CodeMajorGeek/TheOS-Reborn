#include <FileSystem/ext4.h>

bool ext4_check_format(HBA_PORT_t* port)
{
    return FALSE; // FIXME: raise GP.

    char buf[512];
    AHCI_sata_read(port, EXT4_SUPERBLOCK_ADDR / AHCI_SECTOR_SIZE, 0, 1, buf);

    ext4_superblock_t* superblock = (ext4_superblock_t*) buf;
    return superblock->magic == EXT4_MAGIC_SIGNATURE;
}