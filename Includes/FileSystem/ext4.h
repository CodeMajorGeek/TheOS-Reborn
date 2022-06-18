#ifndef _EXT4_H
#define _EXT4_H

#include <stdint.h>

#define EXT4_MAGIC_SIGNATURE    0xEF53
#define EXT4_SUPERBLOCK_ADDR    

typedef struct ext4_dynamic
{
    uint32_t first_nrsv_inode;
    uint16_t size_inode_struct; // In byte.
    uint16_t block_group;
    uint32_t optional_feature;
    uint32_t required_features;
    uint32_t unsupported_features;
    uint8_t file_sys_uuid[16];
    char volume_name[16];
    char path_last_mount[64];
    uint32_t compression_algorithm;
    uint8_t num_blocks_prealloc_files;
    uint8_t num_blocks_prealloc_dirs;
    uint16_t amount_rsv_GDT_fs_exp;
    uint8_t journal_uuid[16];
    uint32_t journal_inode;
    uint32_t journal_device;
    uint32_t head_orphan_inode_list;
    uint32_t HTREE[4];
    uint8_t hash_algorithm_dirs;
    uint8_t journal_blocks;
    uint16_t group_desc_size;
    uint32_t mount_opt;
    uint32_t first_metablock_grp;
    uint32_t fs_creation_time;
    uint32_t journal_inode_backup[17];
} ext4_dynamic;

typedef struct ext4_superblock
{
    uint32_t t_num_inodes;
    uint32_t t_num_blocks;
    uint32_t num_reserved_blocks;
    uint32_t t_num_unalloc_blocks;
    uint32_t t_num_unalloc_inodes;
    uint32_t block_num_superblock;
    uint32_t block_size;    // log2(block size) - 10.
    uint32_t frag_size;     // log2(fragment size) - 10.
    uint32_t num_blocks_grp;
    uint32_t num_frags_grp;
    uint32_t num_inodes_grp;
    uint32_t last_mount_time;
    uint32_t last_written_time;
    uint16_t num_mount_consistency;
    uint16_t num_mount_allowed_consistency;
    uint16_t magic;
    uint16_t file_sys_state;
    uint16_t error_detected;
    uint16_t version_minor;
    uint32_t consistency_time;
    uint32_t forced_consistency_time_interval;
    uint32_t os_id_created;
    uint32_t version_major;
    uint16_t user_id_reserved_blocks;
    uint16_t group_id_reserved_blocks;
    // ext4_extended_t extended_fields;
} ext4_superblock_t;


#endif