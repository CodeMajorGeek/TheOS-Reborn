#ifndef _SATA_H
#define _SATA_H

#include <Device/AHCI.h>

#include <stdint.h>

/* FS types: */
#define DIRECTORY 5
#define FILE_TYPE 0

/* FS organisations: */
#define SUPERBLOCK_SIZE 20
#define SUPERBLOCK_S 1
#define SUPERBLOCK_E 20
#define INODE_S 23
#define INODE_E 151
#define RESERVE_I1_S 152
#define RESERVE_I1_E 160
#define SUPERBLOCK_COPY_S 161
#define SUPERBLOCK_COPY_E 180
#define RESERVE_I2_S 181
#define RESERVE_I2_E 190
#define DATA_S 191
#define DATA_E 40704
#define SECTOR_SIZE 512
#define INODES_PER_SECTOR 3
#define DEFAULT_INDEX_FOR_TABLE 999

/* FS structs: */
typedef struct superblock
{
    char fs_type[10];
    int size;
    int magic_no;
    int inode_start_sector;
    int data_start_sector;
    int free_inode_block[4];
    int free_data_block[1272];
} superblock_t;

typedef struct inode
{
    int inode_num;
    char filename[100];
    int perm;
    int size;
    char type;
    int number_of_dentry;
    int sector_loc[10];
} inode_t;

typedef struct satafs_entry
{
    char inode_num;
    char name[100];
    int size;
    int typeflag;
    int par_ind;
    int number_of_dentry;
} satafs_entry_t;

static satafs_entry_t sata_fs[100];
static int sata_fs_count;

void SATA_init_table(HBA_PORT_t* port);

#endif