#ifndef _DIRENT_H
#define _DIRENT_H

#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

#ifndef NAME_MAX
#define NAME_MAX SYS_DIRENT_NAME_MAX
#endif

#ifndef DT_UNKNOWN
#define DT_UNKNOWN SYS_DT_UNKNOWN
#define DT_DIR     SYS_DT_DIR
#define DT_REG     SYS_DT_REG
#endif

struct dirent
{
    uint32_t d_ino;
    unsigned char d_type;
    char d_name[SYS_DIRENT_NAME_MAX + 1U];
};

typedef struct DIR
{
    char path[256];
    uint64_t index;
    struct dirent current;
    int used;
} DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);
void rewinddir(DIR* dirp);

#endif
