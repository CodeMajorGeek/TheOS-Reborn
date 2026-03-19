#include <errno.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios LibC_termios_state;
static volatile unsigned char LibC_termios_lock = 0;
static volatile unsigned char LibC_termios_ready = 0;

static void termios_lock(void)
{
    while (__atomic_test_and_set(&LibC_termios_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_termios_lock, __ATOMIC_RELAXED) != 0U)
            __asm__ __volatile__("pause");
    }
}

static void termios_unlock(void)
{
    __atomic_clear(&LibC_termios_lock, __ATOMIC_RELEASE);
}

static int termios_fd_supported(int fd)
{
    return isatty(fd) == 1;
}

static void termios_init_defaults_locked(void)
{
    if (__atomic_load_n(&LibC_termios_ready, __ATOMIC_RELAXED) != 0U)
        return;

    memset(&LibC_termios_state, 0, sizeof(LibC_termios_state));

    LibC_termios_state.c_iflag = ICRNL | IXON;
    LibC_termios_state.c_oflag = OPOST | ONLCR;
    LibC_termios_state.c_cflag = CREAD | CS8;
    LibC_termios_state.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    LibC_termios_state.c_cc[VMIN] = 1;
    LibC_termios_state.c_cc[VTIME] = 0;
    LibC_termios_state.c_ispeed = B38400;
    LibC_termios_state.c_ospeed = B38400;

    __atomic_store_n(&LibC_termios_ready, 1, __ATOMIC_RELEASE);
}

int tcgetattr(int fd, struct termios* termios_p)
{
    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }

    if (!termios_fd_supported(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    termios_lock();
    termios_init_defaults_locked();
    *termios_p = LibC_termios_state;
    termios_unlock();

    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios* termios_p)
{
    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }

    if (!termios_fd_supported(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    if (optional_actions != TCSANOW &&
        optional_actions != TCSADRAIN &&
        optional_actions != TCSAFLUSH)
    {
        errno = EINVAL;
        return -1;
    }

    termios_lock();
    termios_init_defaults_locked();
    LibC_termios_state = *termios_p;
    termios_unlock();

    return 0;
}

int tcdrain(int fd)
{
    if (!termios_fd_supported(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    return 0;
}

int tcflush(int fd, int queue_selector)
{
    if (!termios_fd_supported(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    if (queue_selector != TCIFLUSH &&
        queue_selector != TCOFLUSH &&
        queue_selector != TCIOFLUSH)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int tcsendbreak(int fd, int duration)
{
    (void) duration;

    if (!termios_fd_supported(fd))
    {
        errno = ENOTTY;
        return -1;
    }

    return 0;
}

speed_t cfgetispeed(const struct termios* termios_p)
{
    if (!termios_p)
        return 0;

    return termios_p->c_ispeed;
}

speed_t cfgetospeed(const struct termios* termios_p)
{
    if (!termios_p)
        return 0;

    return termios_p->c_ospeed;
}

int cfsetispeed(struct termios* termios_p, speed_t speed)
{
    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }

    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios* termios_p, speed_t speed)
{
    if (!termios_p)
    {
        errno = EINVAL;
        return -1;
    }

    termios_p->c_ospeed = speed;
    return 0;
}

void cfmakeraw(struct termios* termios_p)
{
    if (!termios_p)
        return;

    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
    termios_p->c_cc[VMIN] = 1;
    termios_p->c_cc[VTIME] = 0;
}
