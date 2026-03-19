#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IPC_PRIVATE
#define IPC_PRIVATE ((key_t) 0)
#endif

#ifndef IPC_CREAT
#define IPC_CREAT  01000
#endif
#ifndef IPC_EXCL
#define IPC_EXCL   02000
#endif
#ifndef IPC_NOWAIT
#define IPC_NOWAIT 04000
#endif

key_t ftok(const char* path, int proj_id);

#ifdef __cplusplus
}
#endif

#endif
