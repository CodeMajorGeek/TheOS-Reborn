#include <errno.h>

static __thread int LibC_errno_tls = 0;

int* __errno_location(void)
{
    return &LibC_errno_tls;
}
