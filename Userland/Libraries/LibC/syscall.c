#include <syscall.h>

long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;

    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

int fs_ls(void)
{
    return (int) syscall(SYS_FS_LS, 0, 0, 0, 0, 0, 0);
}

int fs_read(const char* name, void* buf, size_t buf_size, size_t* out_size)
{
    return (int) syscall(SYS_FS_READ, (long) name, (long) buf, (long) buf_size,
                         (long) out_size, 0, 0);
}

int fs_create(const char* name, const void* data, size_t size)
{
    return (int) syscall(SYS_FS_CREATE, (long) name, (long) data, (long) size,
                         0, 0, 0);
}
