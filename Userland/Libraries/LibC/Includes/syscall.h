#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stddef.h>
#include <stdint.h>

enum
{
    SYS_FS_LS = 1,
    SYS_FS_READ = 2,
    SYS_FS_CREATE = 3
};

long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6);

int fs_ls(void);
int fs_read(const char* name, void* buf, size_t buf_size, size_t* out_size);
int fs_create(const char* name, const void* data, size_t size);

#endif
