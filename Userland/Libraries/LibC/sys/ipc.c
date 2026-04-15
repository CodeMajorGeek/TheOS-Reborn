#include <errno.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <syscall.h>

key_t ftok(const char* path, int proj_id)
{
    if (!path)
    {
        errno = EINVAL;
        return (key_t) -1;
    }

    struct stat st;
    if (stat(path, &st) < 0)
        return (key_t) -1;

    uint32_t ino_low = (uint32_t) st.st_ino & 0xFFFFU;
    uint32_t dev_low = (uint32_t) st.st_dev & 0xFFU;
    uint32_t proj_low = (uint32_t) proj_id & 0xFFU;

    return (key_t) (ino_low | (dev_low << 16) | (proj_low << 24));
}

int shmget(key_t key, size_t size, int shmflg)
{
    int rc = sys_shmget((int) key, size, shmflg);
    if (rc < 0)
    {
        errno = ENOSPC;
        return -1;
    }
    return rc;
}

void* shmat(int shmid, const void* shmaddr, int shmflg)
{
    (void) shmflg;
    long rc = sys_shmat(shmid, shmaddr);
    if (rc < 0)
    {
        errno = EINVAL;
        return (void*) -1;
    }
    return (void*) (uintptr_t) rc;
}

int shmdt(const void* shmaddr)
{
    if (sys_shmdt(shmaddr) < 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds* buf)
{
    (void) buf;
    if (sys_shmctl(shmid, cmd) < 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int msgget(key_t key, int msgflg)
{
    int rc = sys_msgget((int) key, msgflg);
    if (rc < 0)
    {
        errno = ENOSPC;
        return -1;
    }
    return rc;
}

int msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg)
{
    int rc = sys_msgsnd(msqid, msgp, msgsz, msgflg);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

ssize_t msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg)
{
    long rc = sys_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = EINVAL;
        return -1;
    }
    return (ssize_t) rc;
}
