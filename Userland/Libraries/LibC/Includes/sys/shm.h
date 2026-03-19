#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <stddef.h>
#include <sys/ipc.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SHM_RDONLY
#define SHM_RDONLY 010000
#endif
#ifndef SHM_RND
#define SHM_RND    020000
#endif
#ifndef SHM_REMAP
#define SHM_REMAP  040000
#endif
#ifndef SHM_EXEC
#define SHM_EXEC   0100000
#endif

#ifndef IPC_RMID
#define IPC_RMID 0
#endif
#ifndef IPC_SET
#define IPC_SET 1
#endif
#ifndef IPC_STAT
#define IPC_STAT 2
#endif

struct shmid_ds
{
    int __unused;
};

int shmget(key_t key, size_t size, int shmflg);
void* shmat(int shmid, const void* shmaddr, int shmflg);
int shmdt(const void* shmaddr);
int shmctl(int shmid, int cmd, struct shmid_ds* buf);

#ifdef __cplusplus
}
#endif

#endif
