#ifndef _SYS_MSG_H
#define _SYS_MSG_H

#include <stddef.h>
#include <sys/ipc.h>

#ifdef __cplusplus
extern "C" {
#endif

struct msgbuf
{
    long mtype;
    char mtext[1];
};

int msgget(key_t key, int msgflg);
int msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg);
ssize_t msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);

#ifdef __cplusplus
}
#endif

#endif
