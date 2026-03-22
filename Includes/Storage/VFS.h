#ifndef _VFS_H
#define _VFS_H

#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_MOUNT_ROOT_PATH "/"
#define VFS_DIRENT_NAME_MAX 255U

#define VFS_DT_UNKNOWN      0U
#define VFS_DT_DIR          4U
#define VFS_DT_REG          8U

typedef struct vfs_dirent_info
{
    uint32_t inode;
    uint8_t type;
    char name[VFS_DIRENT_NAME_MAX + 1U];
} vfs_dirent_info_t;

typedef struct vfs_backend_ops
{
    const char* name;
    bool (*read_file)(const char* path, uint8_t** out_buf, size_t* out_size);
    bool (*write_file)(const char* path, const uint8_t* data, size_t size);
    bool (*create_dir)(const char* path);
    bool (*path_is_dir)(const char* path);
    bool (*read_dirent_at)(const char* path, size_t index, vfs_dirent_info_t* out);
    size_t (*get_block_size)(void);
} vfs_backend_ops_t;

typedef struct vfs_runtime_state
{
    bool lock_ready;
    bool root_mounted;
    spinlock_t lock;
    const vfs_backend_ops_t* root_backend;
} vfs_runtime_state_t;

void VFS_init(void);
bool VFS_mount_root(const vfs_backend_ops_t* backend);
bool VFS_mount_root_ext4(void);
bool VFS_is_ready(void);
const char* VFS_backend_name(void);
size_t VFS_block_size(void);
bool VFS_read_file(const char* path, uint8_t** out_buf, size_t* out_size);
bool VFS_write_file(const char* path, const uint8_t* data, size_t size);
bool VFS_mkdir(const char* path);
bool VFS_path_is_dir(const char* path);
bool VFS_read_dirent_at(const char* path, size_t index, vfs_dirent_info_t* out);


#endif
