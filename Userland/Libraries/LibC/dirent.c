#include <dirent.h>

#include <errno.h>
#include <string.h>

#define DIRENT_MAX_OPEN_DIRS 16U

static DIR Dirent_slots[DIRENT_MAX_OPEN_DIRS];

DIR* opendir(const char* path)
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return NULL;
    }

    if (fs_is_dir(path) != 1)
    {
        errno = ENOTDIR;
        return NULL;
    }

    for (size_t i = 0; i < DIRENT_MAX_OPEN_DIRS; i++)
    {
        if (Dirent_slots[i].used)
            continue;

        size_t len = strlen(path);
        if (len + 1U > sizeof(Dirent_slots[i].path))
        {
            errno = ENAMETOOLONG;
            return NULL;
        }

        memset(&Dirent_slots[i], 0, sizeof(Dirent_slots[i]));
        memcpy(Dirent_slots[i].path, path, len + 1U);
        Dirent_slots[i].index = 0;
        Dirent_slots[i].used = 1;
        return &Dirent_slots[i];
    }

    errno = EMFILE;
    return NULL;
}

struct dirent* readdir(DIR* dirp)
{
    if (!dirp || !dirp->used)
    {
        errno = EBADF;
        return NULL;
    }

    syscall_dirent_t out;
    int rc = fs_readdir(dirp->path, dirp->index, &out);
    if (rc < 0)
    {
        errno = EIO;
        return NULL;
    }
    if (rc == 0)
        return NULL;

    memset(&dirp->current, 0, sizeof(dirp->current));
    dirp->current.d_ino = out.d_ino;
    dirp->current.d_type = out.d_type;
    memcpy(dirp->current.d_name, out.d_name, sizeof(dirp->current.d_name));
    dirp->index++;
    return &dirp->current;
}

int closedir(DIR* dirp)
{
    if (!dirp || !dirp->used)
    {
        errno = EBADF;
        return -1;
    }

    memset(dirp, 0, sizeof(*dirp));
    return 0;
}

void rewinddir(DIR* dirp)
{
    if (!dirp || !dirp->used)
        return;

    dirp->index = 0;
}
