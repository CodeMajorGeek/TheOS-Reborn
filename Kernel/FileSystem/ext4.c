#include <FileSystem/ext4.h>

#include <Memory/KMem.h>

#include <string.h>
#include <stdio.h>
#include <stdint.h>

static ext4_fs_t* ext4_active_fs;

#define EXT4_INODE_MODE_TYPE_MASK    0xF000U
#define EXT4_INODE_MODE_DIRECTORY    0x4000U
#define EXT4_INODE_MODE_REGULAR      0x8000U
#define EXT4_PATH_MAX_COMPONENTS     32U
#define EXT4_PATH_COMPONENT_MAX      255U

static uint16_t ext4_dir_ideal_len(uint8_t name_len)
{
    return (uint16_t) ((8U + name_len + 3U) & ~3U);
}

static bool ext4_dir_entry_is_valid(ext4_fs_t* fs, uint32_t offset, const ext4_dir_entry_t* entry)
{
    if (!entry)
        return false;
    if (entry->rec_len < 8 || (entry->rec_len & 3) != 0)
        return false;
    if (offset + entry->rec_len > fs->block_size)
        return false;
    if (ext4_dir_ideal_len(entry->name_len) > entry->rec_len)
        return false;
    return true;
}

static bool ext4_read_bytes(ext4_fs_t* fs, uint64_t offset, void* out, size_t size)
{
    if (!fs || !fs->port || (!out && size != 0))
        return false;
    if (size == 0)
        return true;

    uint64_t start_lba = fs->lba_base + (offset / AHCI_SECTOR_SIZE);
    uint64_t end_lba = fs->lba_base + ((offset + size + AHCI_SECTOR_SIZE - 1) / AHCI_SECTOR_SIZE);
    if (end_lba <= start_lba || (end_lba - start_lba) > 0xFFFFFFFFULL)
        return false;

    uint32_t sectors = (uint32_t) (end_lba - start_lba);
    size_t byte_count = (size_t) sectors * AHCI_SECTOR_SIZE;

    uint8_t* tmp = (uint8_t*) kmalloc(byte_count);
    if (!tmp)
        return false;

    if (AHCI_sata_read(fs->port, (uint32_t) start_lba, (uint32_t) (start_lba >> 32), sectors, tmp) != 0)
    {
        kfree(tmp);
        return false;
    }

    memcpy(out, tmp + (offset % AHCI_SECTOR_SIZE), size);
    kfree(tmp);
    return true;
}

static bool ext4_write_bytes(ext4_fs_t* fs, uint64_t offset, const void* data, size_t size)
{
    if (!fs || !fs->port || (!data && size != 0))
        return false;
    if (size == 0)
        return true;

    uint64_t start_lba = fs->lba_base + (offset / AHCI_SECTOR_SIZE);
    uint64_t end_lba = fs->lba_base + ((offset + size + AHCI_SECTOR_SIZE - 1) / AHCI_SECTOR_SIZE);
    if (end_lba <= start_lba || (end_lba - start_lba) > 0xFFFFFFFFULL)
        return false;

    uint32_t sectors = (uint32_t) (end_lba - start_lba);
    size_t byte_count = (size_t) sectors * AHCI_SECTOR_SIZE;

    uint8_t* tmp = (uint8_t*) kmalloc(byte_count);
    if (!tmp)
        return false;

    if (AHCI_sata_read(fs->port, (uint32_t) start_lba, (uint32_t) (start_lba >> 32), sectors, tmp) != 0)
    {
        kfree(tmp);
        return false;
    }

    memcpy(tmp + (offset % AHCI_SECTOR_SIZE), data, size);

    if (AHCI_SATA_write(fs->port, (uint32_t) start_lba, (uint32_t) (start_lba >> 32), sectors, tmp) != 0)
    {
        kfree(tmp);
        return false;
    }

    kfree(tmp);
    return true;
}

static bool ext4_read_block(ext4_fs_t* fs, uint32_t block, void* out)
{
    return ext4_read_bytes(fs, (uint64_t) block * fs->block_size, out, fs->block_size);
}

static bool ext4_write_block(ext4_fs_t* fs, uint32_t block, const void* data)
{
    return ext4_write_bytes(fs, (uint64_t) block * fs->block_size, data, fs->block_size);
}

static bool ext4_read_group_desc(ext4_fs_t* fs, uint32_t group, ext4_group_desc_t* out)
{
    uint64_t offset = (uint64_t) fs->gd_table_block * fs->block_size + (uint64_t) group * fs->desc_size;
    return ext4_read_bytes(fs, offset, out, sizeof(*out));
}

static bool ext4_read_inode(ext4_fs_t* fs, uint32_t inode_num, ext4_inode_t* out)
{
    if (inode_num == 0)
        return false;

    uint32_t group = (inode_num - 1) / fs->inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->inodes_per_group;

    ext4_group_desc_t gd;
    if (!ext4_read_group_desc(fs, group, &gd))
        return false;

    uint64_t inode_table_block = gd.bg_inode_table_lo;
    uint64_t offset = inode_table_block * fs->block_size + (uint64_t) index * fs->inode_size;

    uint8_t* tmp = (uint8_t*) kmalloc(fs->inode_size);
    if (!tmp)
        return false;

    if (!ext4_read_bytes(fs, offset, tmp, fs->inode_size))
    {
        kfree(tmp);
        return false;
    }

    memcpy(out, tmp, sizeof(*out));
    kfree(tmp);
    return true;
}

static bool ext4_write_inode(ext4_fs_t* fs, uint32_t inode_num, const ext4_inode_t* inode)
{
    if (inode_num == 0)
        return false;

    uint32_t group = (inode_num - 1) / fs->inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->inodes_per_group;

    ext4_group_desc_t gd;
    if (!ext4_read_group_desc(fs, group, &gd))
        return false;

    uint64_t inode_table_block = gd.bg_inode_table_lo;
    uint64_t offset = inode_table_block * fs->block_size + (uint64_t) index * fs->inode_size;

    uint8_t* tmp = (uint8_t*) kmalloc(fs->inode_size);
    if (!tmp)
        return false;

    memset(tmp, 0, fs->inode_size);
    memcpy(tmp, inode, sizeof(*inode));

    bool ok = ext4_write_bytes(fs, offset, tmp, fs->inode_size);
    kfree(tmp);
    return ok;
}

static bool ext4_inode_get_block(ext4_fs_t* fs, const ext4_inode_t* inode, uint32_t logical_block, uint32_t* phys_block_out)
{
    if (inode->i_flags & EXT4_EXTENTS_FL)
    {
        const ext4_extent_header_t* eh = (const ext4_extent_header_t*) inode->i_block;
        if (eh->eh_magic != EXT4_EXTENT_MAGIC)
            return false;

        if (eh->eh_depth == 0)
        {
            const ext4_extent_t* extents = (const ext4_extent_t*) (inode->i_block + sizeof(ext4_extent_header_t));
            for (uint16_t i = 0; i < eh->eh_entries; ++i)
            {
                uint32_t ee_len = extents[i].ee_len & 0x7FFF;
                if (logical_block >= extents[i].ee_block && logical_block < extents[i].ee_block + ee_len)
                {
                    uint64_t start = ((uint64_t) extents[i].ee_start_hi << 32) | extents[i].ee_start_lo;
                    *phys_block_out = (uint32_t) (start + (logical_block - extents[i].ee_block));
                    return true;
                }
            }
            return false;
        }

        if (eh->eh_depth == 1)
        {
            const ext4_extent_idx_t* idx = (const ext4_extent_idx_t*) (inode->i_block + sizeof(ext4_extent_header_t));
            const ext4_extent_idx_t* chosen = NULL;
            for (uint16_t i = 0; i < eh->eh_entries; ++i)
            {
                if (logical_block < idx[i].ei_block)
                    break;
                chosen = &idx[i];
            }
            if (!chosen)
                return false;

            uint64_t leaf = ((uint64_t) chosen->ei_leaf_hi << 32) | chosen->ei_leaf_lo;
            uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
            if (!block)
                return false;

            if (!ext4_read_block(fs, (uint32_t) leaf, block))
            {
                kfree(block);
                return false;
            }

            const ext4_extent_header_t* leh = (const ext4_extent_header_t*) block;
            if (leh->eh_magic != EXT4_EXTENT_MAGIC || leh->eh_depth != 0)
            {
                kfree(block);
                return false;
            }

            const ext4_extent_t* extents = (const ext4_extent_t*) (block + sizeof(ext4_extent_header_t));
            for (uint16_t i = 0; i < leh->eh_entries; ++i)
            {
                uint32_t ee_len = extents[i].ee_len & 0x7FFF;
                if (logical_block >= extents[i].ee_block && logical_block < extents[i].ee_block + ee_len)
                {
                    uint64_t start = ((uint64_t) extents[i].ee_start_hi << 32) | extents[i].ee_start_lo;
                    *phys_block_out = (uint32_t) (start + (logical_block - extents[i].ee_block));
                    kfree(block);
                    return true;
                }
            }

            kfree(block);
            return false;
        }

        return false;
    }

    const uint32_t* blocks = (const uint32_t*) inode->i_block;
    if (logical_block < 12 && blocks[logical_block] != 0)
    {
        *phys_block_out = blocks[logical_block];
        return true;
    }

    return false;
}

static bool ext4_find_dir_entry(ext4_fs_t* fs, const ext4_inode_t* dir, const char* name, ext4_dir_entry_t* out, uint32_t* out_block)
{
    uint32_t blocks = (dir->i_size_lo + fs->block_size - 1) / fs->block_size;
    uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
    if (!block)
        return false;

    size_t name_len = strlen(name);

    for (uint32_t b = 0; b < blocks; ++b)
    {
        uint32_t phys = 0;
        if (!ext4_inode_get_block(fs, dir, b, &phys))
            continue;
        if (!ext4_read_block(fs, phys, block))
            continue;

        uint32_t offset = 0;
        while (offset < fs->block_size)
        {
            ext4_dir_entry_t* entry = (ext4_dir_entry_t*) (block + offset);
            if (!ext4_dir_entry_is_valid(fs, offset, entry))
                break;

            if (entry->inode != 0 && entry->name_len > 0 &&
                name_len == entry->name_len && memcmp(entry->name, name, entry->name_len) == 0)
            {
                memcpy(out, entry, sizeof(*out));
                if (out_block)
                    *out_block = phys;
                kfree(block);
                return true;
            }

            offset += entry->rec_len;
        }
    }

    kfree(block);
    return false;
}

static bool ext4_resolve_path_inode_impl(ext4_fs_t* fs,
                                         const char* path,
                                         ext4_inode_t* out_inode,
                                         uint32_t* out_inode_num)
{
    if (!fs || !path || !out_inode)
        return false;

    uint32_t current_inode_num = EXT4_INODE_ROOT;
    ext4_inode_t current;
    if (!ext4_read_inode(fs, current_inode_num, &current))
        return false;

    const char* cursor = path;
    while (*cursor == '/')
        cursor++;

    if (*cursor == '\0')
    {
        *out_inode = current;
        if (out_inode_num)
            *out_inode_num = current_inode_num;
        return true;
    }

    while (*cursor != '\0')
    {
        if ((current.i_mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_DIRECTORY)
            return false;

        char component[EXT4_PATH_COMPONENT_MAX + 1U];
        size_t component_len = 0;
        while (*cursor != '\0' && *cursor != '/')
        {
            if (component_len >= sizeof(component) - 1U)
                return false;
            component[component_len++] = *cursor++;
        }
        component[component_len] = '\0';

        while (*cursor == '/')
            cursor++;

        if (component_len == 0)
            continue;

        ext4_dir_entry_t entry;
        if (!ext4_find_dir_entry(fs, &current, component, &entry, NULL))
            return false;

        current_inode_num = entry.inode;
        if (!ext4_read_inode(fs, current_inode_num, &current))
            return false;
    }

    *out_inode = current;
    if (out_inode_num)
        *out_inode_num = current_inode_num;
    return true;
}

static bool ext4_resolve_path_inode(ext4_fs_t* fs, const char* path, ext4_inode_t* out_inode)
{
    return ext4_resolve_path_inode_impl(fs, path, out_inode, NULL);
}

static bool ext4_split_parent_leaf(const char* path,
                                   char* out_parent,
                                   size_t out_parent_size,
                                   char* out_leaf,
                                   size_t out_leaf_size)
{
    if (!path || !out_parent || !out_leaf || out_parent_size < 2U || out_leaf_size < 2U)
        return false;

    char components[EXT4_PATH_MAX_COMPONENTS][EXT4_PATH_COMPONENT_MAX + 1U];
    size_t component_count = 0;
    const char* cursor = path;
    while (*cursor == '/')
        cursor++;

    while (*cursor != '\0')
    {
        char token[EXT4_PATH_COMPONENT_MAX + 1U];
        size_t token_len = 0;
        while (*cursor != '\0' && *cursor != '/')
        {
            if (token_len >= EXT4_PATH_COMPONENT_MAX)
                return false;
            token[token_len++] = *cursor++;
        }
        token[token_len] = '\0';

        while (*cursor == '/')
            cursor++;

        if (token_len == 0)
            continue;
        if (strcmp(token, ".") == 0)
            continue;
        if (strcmp(token, "..") == 0)
        {
            if (component_count > 0U)
                component_count--;
            continue;
        }

        if (component_count >= EXT4_PATH_MAX_COMPONENTS)
            return false;
        memcpy(components[component_count], token, token_len + 1U);
        component_count++;
    }

    if (component_count == 0)
        return false;

    size_t leaf_len = strlen(components[component_count - 1U]);
    if (leaf_len >= out_leaf_size)
        return false;
    memcpy(out_leaf, components[component_count - 1U], leaf_len + 1U);

    size_t len = 0;
    out_parent[len++] = '/';
    out_parent[len] = '\0';
    for (size_t i = 0; i + 1U < component_count; i++)
    {
        size_t part_len = strlen(components[i]);
        if (len > 1U)
        {
            if (len + 1U >= out_parent_size)
                return false;
            out_parent[len++] = '/';
        }

        if (len + part_len >= out_parent_size)
            return false;
        memcpy(out_parent + len, components[i], part_len);
        len += part_len;
        out_parent[len] = '\0';
    }

    return true;
}

static bool ext4_dir_insert_entry(ext4_fs_t* fs,
                                  ext4_inode_t* dir,
                                  const char* name,
                                  uint32_t inode_num,
                                  uint8_t file_type)
{
    if (!fs || !dir || !name || name[0] == '\0')
        return false;
    if ((dir->i_mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_DIRECTORY)
        return false;

    size_t name_len = strlen(name);
    if (name_len > EXT4_PATH_COMPONENT_MAX)
        return false;

    uint32_t dir_block_num = 0;
    if (!ext4_inode_get_block(fs, dir, 0, &dir_block_num))
        return false;

    uint8_t* dir_block = (uint8_t*) kmalloc(fs->block_size);
    if (!dir_block)
        return false;
    if (!ext4_read_block(fs, dir_block_num, dir_block))
    {
        kfree(dir_block);
        return false;
    }

    uint16_t needed_len = ext4_dir_ideal_len((uint8_t) name_len);
    uint32_t offset = 0;
    bool inserted = false;
    while (offset < fs->block_size)
    {
        ext4_dir_entry_t* entry = (ext4_dir_entry_t*) (dir_block + offset);
        if (!ext4_dir_entry_is_valid(fs, offset, entry))
            break;

        uint16_t ideal = ext4_dir_ideal_len(entry->name_len);
        if (entry->rec_len >= ideal && (uint16_t) (entry->rec_len - ideal) >= needed_len)
        {
            uint16_t remaining = entry->rec_len - ideal;
            entry->rec_len = ideal;

            ext4_dir_entry_t* new_entry = (ext4_dir_entry_t*) (dir_block + offset + ideal);
            new_entry->inode = inode_num;
            new_entry->rec_len = remaining;
            new_entry->name_len = (uint8_t) name_len;
            new_entry->file_type = file_type;
            memcpy(new_entry->name, name, name_len);
            inserted = true;
            break;
        }

        offset += entry->rec_len;
    }

    bool ok = false;
    if (inserted)
        ok = ext4_write_block(fs, dir_block_num, dir_block);
    kfree(dir_block);
    return ok;
}

static bool ext4_alloc_from_bitmap(ext4_fs_t* fs, uint32_t bitmap_block, uint32_t start_bit, uint32_t max_bits, uint32_t* out_index)
{
    uint8_t* bitmap = (uint8_t*) kmalloc(fs->block_size);
    if (!bitmap)
        return false;

    if (!ext4_read_block(fs, bitmap_block, bitmap))
    {
        kfree(bitmap);
        return false;
    }

    uint32_t bitmap_bits = fs->block_size * 8;
    if (max_bits > bitmap_bits)
        max_bits = bitmap_bits;

    for (uint32_t bit = start_bit; bit < max_bits; ++bit)
    {
        uint32_t byte = bit / 8;
        uint8_t mask = (uint8_t) (1U << (bit % 8));
        if ((bitmap[byte] & mask) == 0)
        {
            bitmap[byte] |= mask;
            if (!ext4_write_block(fs, bitmap_block, bitmap))
            {
                kfree(bitmap);
                return false;
            }

            *out_index = bit;
            kfree(bitmap);
            return true;
        }
    }

    kfree(bitmap);
    return false;
}

static bool ext4_alloc_block(ext4_fs_t* fs, uint32_t* out_block)
{
    ext4_group_desc_t gd;
    if (!ext4_read_group_desc(fs, 0, &gd))
        return false;

    uint32_t index = 0;
    if (!ext4_alloc_from_bitmap(fs, gd.bg_block_bitmap_lo, 0, fs->blocks_per_group, &index))
        return false;

    *out_block = fs->first_data_block + index;

    gd.bg_free_blocks_count_lo--;
    if (!ext4_write_bytes(fs, (uint64_t) fs->gd_table_block * fs->block_size, &gd, sizeof(gd)))
        return false;

    fs->superblock.s_free_blocks_count_lo--;
    if (!ext4_write_bytes(fs, EXT4_SUPERBLOCK_ADDR, &fs->superblock, sizeof(fs->superblock)))
        return false;

    return true;
}

static bool ext4_alloc_inode(ext4_fs_t* fs, uint32_t* out_inode)
{
    ext4_group_desc_t gd;
    if (!ext4_read_group_desc(fs, 0, &gd))
        return false;

    uint32_t index = 0;
    uint32_t start = fs->superblock.s_first_ino ? (fs->superblock.s_first_ino - 1) : 0;
    if (!ext4_alloc_from_bitmap(fs, gd.bg_inode_bitmap_lo, start, fs->inodes_per_group, &index))
        return false;

    *out_inode = index + 1;

    gd.bg_free_inodes_count_lo--;
    if (!ext4_write_bytes(fs, (uint64_t) fs->gd_table_block * fs->block_size, &gd, sizeof(gd)))
        return false;

    fs->superblock.s_free_inodes_count--;
    if (!ext4_write_bytes(fs, EXT4_SUPERBLOCK_ADDR, &fs->superblock, sizeof(fs->superblock)))
        return false;

    return true;
}

static bool ext4_update_existing_file(ext4_fs_t* fs, uint32_t inode_num, const uint8_t* data, size_t size)
{
    ext4_inode_t inode;
    if (!ext4_read_inode(fs, inode_num, &inode))
        return false;

    uint32_t data_block = 0;
    if (!ext4_inode_get_block(fs, &inode, 0, &data_block))
    {
        if (!ext4_alloc_block(fs, &data_block))
            return false;

        inode.i_flags = EXT4_EXTENTS_FL;
        memset(inode.i_block, 0, sizeof(inode.i_block));

        ext4_extent_header_t* eh = (ext4_extent_header_t*) inode.i_block;
        eh->eh_magic = EXT4_EXTENT_MAGIC;
        eh->eh_entries = 1;
        eh->eh_max = 4;
        eh->eh_depth = 0;
        eh->eh_generation = 0;

        ext4_extent_t* ex = (ext4_extent_t*) (inode.i_block + sizeof(ext4_extent_header_t));
        ex->ee_block = 0;
        ex->ee_len = 1;
        ex->ee_start_hi = (uint16_t) (((uint64_t) data_block) >> 32);
        ex->ee_start_lo = data_block;
    }

    uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
    if (!block)
        return false;

    memset(block, 0, fs->block_size);
    if (size > 0)
        memcpy(block, data, size);

    if (!ext4_write_block(fs, data_block, block))
    {
        kfree(block);
        return false;
    }
    kfree(block);

    inode.i_size_lo = (uint32_t) size;
    inode.i_size_high = 0;
    inode.i_blocks_lo = fs->block_size / AHCI_SECTOR_SIZE;
    return ext4_write_inode(fs, inode_num, &inode);
}

bool ext4_check_format(HBA_PORT_t* port)
{
    uint8_t buf[1024];
    if (AHCI_sata_read(port, EXT4_SUPERBLOCK_ADDR / AHCI_SECTOR_SIZE, 0, 2, buf) != 0)
        return false;

    ext4_superblock_t* superblock = (ext4_superblock_t*) buf;
    return superblock->s_magic == EXT4_MAGIC_SIGNATURE;
}

bool ext4_mount(ext4_fs_t* fs, HBA_PORT_t* port)
{
    return ext4_mount_lba(fs, port, 0);
}

bool ext4_mount_lba(ext4_fs_t* fs, HBA_PORT_t* port, uint64_t lba_base)
{
    if (!fs || !port)
        return false;

    memset(fs, 0, sizeof(*fs));
    fs->port = port;
    fs->lba_base = lba_base;

    if (!ext4_read_bytes(fs, EXT4_SUPERBLOCK_ADDR, &fs->superblock, sizeof(fs->superblock)))
        return false;
    if (fs->superblock.s_magic != EXT4_MAGIC_SIGNATURE)
        return false;

    fs->block_size = 1024U << fs->superblock.s_log_block_size;
    fs->inode_size = fs->superblock.s_inode_size ? fs->superblock.s_inode_size : 128;
    fs->blocks_per_group = fs->superblock.s_blocks_per_group;
    fs->inodes_per_group = fs->superblock.s_inodes_per_group;
    fs->first_data_block = fs->superblock.s_first_data_block;
    fs->desc_size = fs->superblock.s_desc_size ? fs->superblock.s_desc_size : sizeof(ext4_group_desc_t);
    fs->gd_table_block = (fs->block_size == 1024) ? 2 : 1;

    if (fs->block_size < 1024 || (fs->block_size & (fs->block_size - 1)) != 0)
        return false;
    if (fs->inode_size < sizeof(ext4_inode_t) || fs->inode_size > fs->block_size)
        return false;
    if (fs->blocks_per_group == 0 || fs->inodes_per_group == 0)
        return false;

    return true;
}

void ext4_set_active(ext4_fs_t* fs)
{
    ext4_active_fs = fs;
}

ext4_fs_t* ext4_get_active(void)
{
    return ext4_active_fs;
}

bool ext4_list_root(ext4_fs_t* fs)
{
    return ext4_list_path(fs, "/");
}

bool ext4_list_path(ext4_fs_t* fs, const char* path)
{
    if (!fs || !path)
        return false;

    ext4_inode_t root;
    if (!ext4_resolve_path_inode(fs, path, &root))
        return false;
    if ((root.i_mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_DIRECTORY)
        return false;

    uint32_t blocks = (root.i_size_lo + fs->block_size - 1) / fs->block_size;
    uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
    if (!block)
        return false;

    const char* label = path[0] ? path : "/";
    printf("ext4 %s:\n", label);
    for (uint32_t b = 0; b < blocks; ++b)
    {
        uint32_t phys = 0;
        if (!ext4_inode_get_block(fs, &root, b, &phys))
            continue;
        if (!ext4_read_block(fs, phys, block))
            continue;

        uint32_t offset = 0;
        while (offset < fs->block_size)
        {
            ext4_dir_entry_t* entry = (ext4_dir_entry_t*) (block + offset);
            if (!ext4_dir_entry_is_valid(fs, offset, entry))
                break;

            if (entry->inode != 0 && entry->name_len > 0)
            {
                char name[256];
                size_t len = entry->name_len;
                if (len > sizeof(name) - 1)
                    len = sizeof(name) - 1;
                memcpy(name, entry->name, len);
                name[len] = '\0';
                printf("  %s\n", name);
            }

            offset += entry->rec_len;
        }
    }

    kfree(block);
    return true;
}

bool ext4_path_is_dir(ext4_fs_t* fs, const char* path)
{
    if (!fs || !path)
        return false;

    ext4_inode_t inode;
    if (!ext4_resolve_path_inode(fs, path, &inode))
        return false;

    return (inode.i_mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_DIRECTORY;
}

bool ext4_read_file(ext4_fs_t* fs, const char* name, uint8_t** out_buf, size_t* out_size)
{
    if (!fs || !name || !out_buf || !out_size)
        return false;

    *out_buf = NULL;
    *out_size = 0;

    ext4_inode_t inode;
    if (!ext4_resolve_path_inode(fs, name, &inode))
        return false;
    if ((inode.i_mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_DIRECTORY)
        return false;

    size_t size = inode.i_size_lo;
    uint8_t* buf = (uint8_t*) kmalloc(size + 1);
    if (!buf)
        return false;

    size_t read = 0;
    uint32_t block_index = 0;
    while (read < size)
    {
        uint32_t phys = 0;
        if (!ext4_inode_get_block(fs, &inode, block_index, &phys))
            break;

        uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
        if (!block)
            break;
        if (!ext4_read_block(fs, phys, block))
        {
            kfree(block);
            break;
        }

        size_t to_copy = fs->block_size;
        if (size - read < to_copy)
            to_copy = size - read;
        memcpy(buf + read, block, to_copy);
        kfree(block);

        read += to_copy;
        block_index++;
    }

    if (read != size)
    {
        kfree(buf);
        return false;
    }

    buf[size] = '\0';
    *out_buf = buf;
    *out_size = size;
    return true;
}

bool ext4_create_file(ext4_fs_t* fs, const char* name, const uint8_t* data, size_t size)
{
    if (!fs || !name || name[0] == '\0')
        return false;
    if (!data && size != 0)
        return false;
    if (size > fs->block_size)
        return false;

    char parent_path[256];
    char leaf[EXT4_PATH_COMPONENT_MAX + 1U];
    if (!ext4_split_parent_leaf(name, parent_path, sizeof(parent_path), leaf, sizeof(leaf)))
        return false;

    ext4_inode_t parent;
    if (!ext4_resolve_path_inode_impl(fs, parent_path, &parent, NULL))
        return false;
    if ((parent.i_mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_DIRECTORY)
        return false;

    ext4_dir_entry_t existing;
    if (ext4_find_dir_entry(fs, &parent, leaf, &existing, NULL))
    {
        ext4_inode_t existing_inode;
        if (!ext4_read_inode(fs, existing.inode, &existing_inode))
            return false;
        if ((existing_inode.i_mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_DIRECTORY)
            return false;
        return ext4_update_existing_file(fs, existing.inode, data, size);
    }

    uint32_t new_inode = 0;
    if (!ext4_alloc_inode(fs, &new_inode))
        return false;

    uint32_t data_block = 0;
    if (!ext4_alloc_block(fs, &data_block))
        return false;

    uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
    if (!block)
        return false;

    memset(block, 0, fs->block_size);
    if (size > 0)
        memcpy(block, data, size);
    if (!ext4_write_block(fs, data_block, block))
    {
        kfree(block);
        return false;
    }
    kfree(block);

    ext4_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT4_INODE_MODE_REGULAR | 0644;
    inode.i_links_count = 1;
    inode.i_size_lo = (uint32_t) size;
    inode.i_blocks_lo = fs->block_size / AHCI_SECTOR_SIZE;
    inode.i_flags = EXT4_EXTENTS_FL;

    ext4_extent_header_t* eh = (ext4_extent_header_t*) inode.i_block;
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;
    eh->eh_generation = 0;

    ext4_extent_t* ex = (ext4_extent_t*) (inode.i_block + sizeof(ext4_extent_header_t));
    ex->ee_block = 0;
    ex->ee_len = 1;
    ex->ee_start_hi = (uint16_t) (((uint64_t) data_block) >> 32);
    ex->ee_start_lo = data_block;

    if (!ext4_write_inode(fs, new_inode, &inode))
        return false;

    return ext4_dir_insert_entry(fs, &parent, leaf, new_inode, EXT4_FT_REG_FILE);
}

bool ext4_create_dir(ext4_fs_t* fs, const char* path)
{
    if (!fs || !path || path[0] == '\0')
        return false;

    char parent_path[256];
    char leaf[EXT4_PATH_COMPONENT_MAX + 1U];
    if (!ext4_split_parent_leaf(path, parent_path, sizeof(parent_path), leaf, sizeof(leaf)))
        return false;

    ext4_inode_t parent;
    uint32_t parent_inode_num = 0;
    if (!ext4_resolve_path_inode_impl(fs, parent_path, &parent, &parent_inode_num))
        return false;
    if ((parent.i_mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_DIRECTORY)
        return false;

    ext4_dir_entry_t existing;
    if (ext4_find_dir_entry(fs, &parent, leaf, &existing, NULL))
        return false;

    uint32_t new_inode_num = 0;
    if (!ext4_alloc_inode(fs, &new_inode_num))
        return false;

    uint32_t new_block_num = 0;
    if (!ext4_alloc_block(fs, &new_block_num))
        return false;

    uint8_t* block = (uint8_t*) kmalloc(fs->block_size);
    if (!block)
        return false;
    memset(block, 0, fs->block_size);

    uint16_t dot_len = ext4_dir_ideal_len(1);
    ext4_dir_entry_t* dot = (ext4_dir_entry_t*) block;
    dot->inode = new_inode_num;
    dot->rec_len = dot_len;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0] = '.';

    ext4_dir_entry_t* dotdot = (ext4_dir_entry_t*) (block + dot_len);
    dotdot->inode = parent_inode_num;
    dotdot->rec_len = (uint16_t) (fs->block_size - dot_len);
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    bool write_block_ok = ext4_write_block(fs, new_block_num, block);
    kfree(block);
    if (!write_block_ok)
        return false;

    ext4_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT4_INODE_MODE_DIRECTORY | 0755;
    inode.i_links_count = 2;
    inode.i_size_lo = fs->block_size;
    inode.i_blocks_lo = fs->block_size / AHCI_SECTOR_SIZE;
    inode.i_flags = EXT4_EXTENTS_FL;

    ext4_extent_header_t* eh = (ext4_extent_header_t*) inode.i_block;
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;
    eh->eh_generation = 0;

    ext4_extent_t* ex = (ext4_extent_t*) (inode.i_block + sizeof(ext4_extent_header_t));
    ex->ee_block = 0;
    ex->ee_len = 1;
    ex->ee_start_hi = (uint16_t) (((uint64_t) new_block_num) >> 32);
    ex->ee_start_lo = new_block_num;

    if (!ext4_write_inode(fs, new_inode_num, &inode))
        return false;
    if (!ext4_dir_insert_entry(fs, &parent, leaf, new_inode_num, EXT4_FT_DIR))
        return false;

    parent.i_links_count++;
    (void) ext4_write_inode(fs, parent_inode_num, &parent);
    return true;
}
