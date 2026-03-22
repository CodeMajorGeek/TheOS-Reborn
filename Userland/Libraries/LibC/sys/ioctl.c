#include <errno.h>
#include <libc_fd.h>
#include <stdarg.h>
#include <stddef.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <UAPI/DRM.h>
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

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        if (request == FIONREAD)
        {
            if (!argp)
            {
                errno = EFAULT;
                return -1;
            }
            *(int*) argp = 0;
            return 0;
        }

        if (request == FIONBIO)
        {
            if (!argp)
            {
                errno = EFAULT;
                return -1;
            }
            errno = ENOTSUP;
            return -1;
        }

        errno = ENOTTY;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(fd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE)
    {
        if (!argp)
        {
            errno = EFAULT;
            return -1;
        }
        drm_prime_handle_t local;
        local = *(drm_prime_handle_t*) argp;
        int kernel_dma_fd = libc_fd_get_kernel(local.fd);
        if (kernel_dma_fd < 0)
        {
            errno = EBADF;
            return -1;
        }

        local.fd = kernel_dma_fd;
        if (sys_ioctl(kernel_fd, request, &local) < 0)
        {
            errno = EINVAL;
            return -1;
        }

        *(drm_prime_handle_t*) argp = local;
        return 0;
    }

    if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD)
    {
        if (!argp)
        {
            errno = EFAULT;
            return -1;
        }
        drm_prime_handle_t local;
        local = *(drm_prime_handle_t*) argp;
        if (sys_ioctl(kernel_fd, request, &local) < 0)
        {
            errno = EINVAL;
            return -1;
        }

        int user_dma_fd = libc_fd_adopt_kernel(local.fd);
        if (user_dma_fd < 0)
        {
            (void) sys_close(local.fd);
            errno = EMFILE;
            return -1;
        }

        local.fd = user_dma_fd;
        *(drm_prime_handle_t*) argp = local;
        return 0;
    }

    if (sys_ioctl(kernel_fd, request, argp) < 0)
    {
        errno = ENOTTY;
        return -1;
    }

    return 0;
}
