#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void* argp = NULL;

    va_start(ap, request);
    argp = va_arg(ap, void*);
    va_end(ap);

    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    switch (request)
    {
        case FIONREAD:
        {
            if (!argp)
            {
                errno = EFAULT;
                return -1;
            }

            /*
             * Non-blocking byte-availability query is not yet wired to a
             * kernel-side poll API. Report 0 bytes available by default.
             */
            *(int*) argp = 0;
            return 0;
        }

        case FIONBIO:
        {
            if (!argp)
            {
                errno = EFAULT;
                return -1;
            }

            /*
             * Files/socket non-blocking flag toggling is currently unsupported.
             * Keep a deterministic contract for callers.
             */
            errno = ENOTSUP;
            return -1;
        }

        default:
            break;
    }

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        errno = ENOTTY;
        return -1;
    }

    errno = ENOSYS;
    return -1;
}
