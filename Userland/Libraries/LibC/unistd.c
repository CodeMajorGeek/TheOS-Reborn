#include <unistd.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <syscall.h>

ssize_t read(int fd, void* buf, size_t count)
{
    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd == STDIN_FILENO)
    {
        errno = ENOSYS;
        return -1;
    }

    int rc = sys_read(fd, buf, count);
    if (rc < 0)
    {
        errno = EIO;
        return -1;
    }

    return (ssize_t) rc;
}

ssize_t write(int fd, const void* buf, size_t count)
{
    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        int rc = sys_console_write(buf, count);
        if (rc < 0)
        {
            errno = EIO;
            return -1;
        }
        return (ssize_t) rc;
    }

    int rc = sys_write(fd, buf, count);
    if (rc < 0)
    {
        errno = EIO;
        return -1;
    }

    return (ssize_t) rc;
}

int close(int fd)
{
    int rc = sys_close(fd);
    if (rc < 0)
    {
        errno = EBADF;
        return -1;
    }

    return 0;
}

off_t lseek(int fd, off_t offset, int whence)
{
    int64_t rc = sys_lseek(fd, (int64_t) offset, whence);
    if (rc < 0)
    {
        errno = EINVAL;
        return (off_t) -1;
    }

    return (off_t) rc;
}

int isatty(int fd)
{
    return (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) ? 1 : 0;
}

unsigned int sleep(unsigned int seconds)
{
    uint64_t remaining_ms = (uint64_t) seconds * 1000ULL;
    while (remaining_ms > 0)
    {
        uint32_t chunk = (remaining_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t) remaining_ms;
        if (sys_sleep_ms(chunk) < 0)
            return (unsigned int) ((remaining_ms + 999ULL) / 1000ULL);
        remaining_ms -= chunk;
    }

    return 0;
}

int usleep(unsigned int usec)
{
    uint32_t ms = (usec == 0U) ? 0U : (uint32_t) (((uint64_t) usec + 999ULL) / 1000ULL);
    if (sys_sleep_ms(ms) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int sleep_state(unsigned int state)
{
    if (state > SYS_SLEEP_STATE_S5)
    {
        errno = EINVAL;
        return -1;
    }

    if (sys_sleep(state) < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

int shutdown(void)
{
    if (sys_shutdown() < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

int reboot(void)
{
    if (sys_reboot() < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}
