#include <errno.h>

static int libc_errno = 0;

int* __errno_location(void)
{
    return &libc_errno;
}
