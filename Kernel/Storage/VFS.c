#include <Storage/VFS.h>

#include <FileSystem/ext4.h>

#include <string.h>

static vfs_runtime_state_t VFS_state;

static uint8_t VFS_dirent_type_from_ext4(uint8_t ext4_type)
{
    switch (ext4_type)
    {
        case EXT4_FT_DIR:
            return VFS_DT_DIR;
        case EXT4_FT_REG_FILE:
            return VFS_DT_REG;
        default:
            return VFS_DT_UNKNOWN;
    }
}

static bool VFS_ext4_read_file(const char* path, uint8_t** out_buf, size_t* out_size)
{
    if (!path || !out_buf || !out_size)
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return false;
    return ext4_read_file(fs, path, out_buf, out_size);
}

static bool VFS_ext4_write_file(const char* path, const uint8_t* data, size_t size)
{
    if (!path || (size != 0U && !data))
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return false;
    return ext4_create_file(fs, path, data, size);
}

static bool VFS_ext4_create_dir(const char* path)
{
    if (!path)
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return false;
    return ext4_create_dir(fs, path);
}

static bool VFS_ext4_path_is_dir(const char* path)
{
    if (!path)
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return false;
    return ext4_path_is_dir(fs, path);
}

static bool VFS_ext4_read_dirent_at(const char* path, size_t index, vfs_dirent_info_t* out)
{
    if (!path || !out)
        return false;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return false;

    ext4_dirent_info_t ext4_info;
    memset(&ext4_info, 0, sizeof(ext4_info));
    if (!ext4_read_dirent_at(fs, path, index, &ext4_info))
        return false;

    memset(out, 0, sizeof(*out));
    out->inode = ext4_info.inode;
    out->type = VFS_dirent_type_from_ext4(ext4_info.file_type);
    size_t name_len = strlen(ext4_info.name);
    if (name_len > VFS_DIRENT_NAME_MAX)
        name_len = VFS_DIRENT_NAME_MAX;
    memcpy(out->name, ext4_info.name, name_len);
    out->name[name_len] = '\0';
    return true;
}

static size_t VFS_ext4_get_block_size(void)
{
    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return 0U;
    return fs->block_size;
}

static const vfs_backend_ops_t VFS_ext4_backend = {
    .name = "ext4",
    .read_file = VFS_ext4_read_file,
    .write_file = VFS_ext4_write_file,
    .create_dir = VFS_ext4_create_dir,
    .path_is_dir = VFS_ext4_path_is_dir,
    .read_dirent_at = VFS_ext4_read_dirent_at,
    .get_block_size = VFS_ext4_get_block_size
};

void VFS_init(void)
{
    if (!VFS_state.lock_ready)
    {
        spinlock_init(&VFS_state.lock);
        VFS_state.lock_ready = true;
    }

    spin_lock(&VFS_state.lock);
    if (!VFS_state.root_mounted)
        VFS_state.root_backend = NULL;
    spin_unlock(&VFS_state.lock);
}

bool VFS_mount_root(const vfs_backend_ops_t* backend)
{
    if (!backend ||
        !backend->name ||
        !backend->read_file ||
        !backend->write_file ||
        !backend->create_dir ||
        !backend->path_is_dir ||
        !backend->read_dirent_at ||
        !backend->get_block_size)
    {
        return false;
    }

    VFS_init();
    spin_lock(&VFS_state.lock);
    VFS_state.root_backend = backend;
    VFS_state.root_mounted = true;
    spin_unlock(&VFS_state.lock);
    return true;
}

bool VFS_mount_root_ext4(void)
{
    return VFS_mount_root(&VFS_ext4_backend);
}

bool VFS_is_ready(void)
{
    return VFS_state.root_mounted && VFS_state.root_backend != NULL;
}

const char* VFS_backend_name(void)
{
    if (!VFS_is_ready())
        return NULL;
    return VFS_state.root_backend->name;
}

size_t VFS_block_size(void)
{
    if (!VFS_is_ready())
        return 0U;
    return VFS_state.root_backend->get_block_size();
}

bool VFS_read_file(const char* path, uint8_t** out_buf, size_t* out_size)
{
    if (!VFS_is_ready())
        return false;
    return VFS_state.root_backend->read_file(path, out_buf, out_size);
}

bool VFS_write_file(const char* path, const uint8_t* data, size_t size)
{
    if (!VFS_is_ready())
        return false;
    return VFS_state.root_backend->write_file(path, data, size);
}

bool VFS_mkdir(const char* path)
{
    if (!VFS_is_ready())
        return false;
    return VFS_state.root_backend->create_dir(path);
}

bool VFS_path_is_dir(const char* path)
{
    if (!VFS_is_ready())
        return false;
    return VFS_state.root_backend->path_is_dir(path);
}

bool VFS_read_dirent_at(const char* path, size_t index, vfs_dirent_info_t* out)
{
    if (!VFS_is_ready())
        return false;
    return VFS_state.root_backend->read_dirent_at(path, index, out);
}
