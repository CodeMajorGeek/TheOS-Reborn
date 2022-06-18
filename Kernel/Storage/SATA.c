#include <Storage/SATA.h>

#include <Memory/VMM.h>
#include <FileSystem/ext4.h>

#include <stdio.h>

static uint16_t* AHCI_buffer;

void SATA_init_table(HBA_PORT_t* port)
{
    AHCI_buffer = (uint16_t*) VMM_get_AHCI_buffer_virt();
    printf("AHCI_buffer = 0x%X !\n", AHCI_buffer);

    if (!AHCI_read(port, 1, 0, 20, (uint16_t*) AHCI_buffer))
    {
        printf("Can't read superblock using AHCI !\n");
        return;
    }

    printf("Value at 0x400 = %X !\n", AHCI_buffer[0x410]);

    while (1);

    ext4_superblock_t* ext4_sb = (ext4_superblock_t*) AHCI_buffer;
    
    printf ("Is a ext4 superblock : %B !\n", ext4_sb->magic == EXT4_MAGIC_SIGNATURE);

    printf("Successfully initialized SATA !\n");
}