#include <dirent.h>

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define DIRENT_MAX_OPEN_DIRS 16U
#define DIRENT_SPIN_BEFORE_YIELD 256U

static DIR Dirent_slots[DIRENT_MAX_OPEN_DIRS];
static volatile uint8_t Dirent_lock_flag = 0;

static void dirent_lock(void)
{
    unsigned int spins = 0U;
    while (__atomic_test_and_set(&Dirent_lock_flag, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&Dirent_lock_flag, __ATOMIC_RELAXED) != 0U)
        {
            spins++;
            if (spins >= DIRENT_SPIN_BEFORE_YIELD)
            {
                spins = 0U;
                (void) sys_yield();
            }
        }
    }
}

static void dirent_unlock(void)
{
    __atomic_clear(&Dirent_lock_flag, __ATOMIC_RELEASE);
}

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

    dirent_lock();
    for (size_t i = 0; i < DIRENT_MAX_OPEN_DIRS; i++)
    {
        if (Dirent_slots[i].used)
            continue;

        size_t len = strlen(path);
        if (len + 1U > sizeof(Dirent_slots[i].path))
        {
            dirent_unlock();
            errno = ENAMETOOLONG;
            return NULL;
        }

        memset(&Dirent_slots[i], 0, sizeof(Dirent_slots[i]));
        memcpy(Dirent_slots[i].path, path, len + 1U);
        Dirent_slots[i].index = 0;
        Dirent_slots[i].used = 1;
        dirent_unlock();
        return &Dirent_slots[i];
    }
    dirent_unlock();

    errno = EMFILE;
    return NULL;
}

struct dirent* readdir(DIR* dirp)
{
    dirent_lock();
    if (!dirp || !dirp->used)
    {
        dirent_unlock();
        errno = EBADF;
        return NULL;
    }

    syscall_dirent_t out;
    int rc = fs_readdir(dirp->path, dirp->index, &out);
    if (rc < 0)
    {
        dirent_unlock();
        errno = EIO;
        return NULL;
    }
    if (rc == 0)
    {
        dirent_unlock();
        return NULL;
    }

    memset(&dirp->current, 0, sizeof(dirp->current));
    dirp->current.d_ino = out.d_ino;
    dirp->current.d_type = out.d_type;
    memcpy(dirp->current.d_name, out.d_name, sizeof(dirp->current.d_name));
    dirp->index++;
    dirent_unlock();
    return &dirp->current;
}

int closedir(DIR* dirp)
{
    dirent_lock();
    if (!dirp || !dirp->used)
    {
        dirent_unlock();
        errno = EBADF;
        return -1;
    }

    memset(dirp, 0, sizeof(*dirp));
    dirent_unlock();
    return 0;
}

void rewinddir(DIR* dirp)
{
    dirent_lock();
    if (!dirp || !dirp->used)
    {
        dirent_unlock();
        return;
    }

    dirp->index = 0;
    dirent_unlock();
}
