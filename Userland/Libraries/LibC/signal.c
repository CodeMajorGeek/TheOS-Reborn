#include <errno.h>
#include <signal.h>
#include <syscall.h>
#include <unistd.h>

#define LIBC_SIGNAL_MAX NSIG

static sighandler_t LibC_signal_handlers[LIBC_SIGNAL_MAX];
static volatile unsigned char LibC_signal_lock = 0U;
static volatile unsigned char LibC_signal_init_done = 0U;

static void signal_lock(void)
{
    while (__atomic_test_and_set(&LibC_signal_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_signal_lock, __ATOMIC_RELAXED) != 0U)
            __asm__ __volatile__("pause");
    }
}

static void signal_unlock(void)
{
    __atomic_clear(&LibC_signal_lock, __ATOMIC_RELEASE);
}

static void signal_init_handlers_locked(void)
{
    if (__atomic_load_n(&LibC_signal_init_done, __ATOMIC_RELAXED) != 0U)
        return;

    for (int i = 0; i < LIBC_SIGNAL_MAX; i++)
        LibC_signal_handlers[i] = SIG_DFL;

    __atomic_store_n(&LibC_signal_init_done, 1U, __ATOMIC_RELEASE);
}

static int signal_is_valid_number(int sig)
{
    return (sig > 0 && sig < NSIG);
}

sighandler_t signal(int sig, sighandler_t handler)
{
    if (!signal_is_valid_number(sig) || handler == SIG_ERR)
    {
        errno = EINVAL;
        return SIG_ERR;
    }

    if (sig == SIGKILL || sig == SIGSTOP)
    {
        errno = EINVAL;
        return SIG_ERR;
    }

    signal_lock();
    signal_init_handlers_locked();
    sighandler_t previous = LibC_signal_handlers[sig];
    LibC_signal_handlers[sig] = handler;
    signal_unlock();

    return previous;
}

int raise(int sig)
{
    if (!signal_is_valid_number(sig))
    {
        errno = EINVAL;
        return -1;
    }

    signal_lock();
    signal_init_handlers_locked();
    sighandler_t handler = LibC_signal_handlers[sig];
    signal_unlock();

    if (handler == SIG_IGN)
        return 0;

    if (handler == SIG_DFL)
    {
        int self_tid = sys_thread_self();
        if (self_tid <= 0)
        {
            errno = ESRCH;
            return -1;
        }

        int rc = sys_kill(self_tid, sig);
        if (rc < 0)
        {
            errno = ESRCH;
            return -1;
        }
        return 0;
    }

    handler(sig);
    return 0;
}
