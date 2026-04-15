#include <errno.h>
#include <fcntl.h>
#include <libc_fd.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIBC_EXEC_ARG_MAX  64U
#define LIBC_EXEC_PATH_MAX 256U
#define LIBC_FD_BASE       3
#define LIBC_FD_SLOTS      128U
#define LIBC_STAT_BLKSIZE  512L
#define LIBC_BRK_PAGE_SIZE 0x1000UL
#define LIBC_BRK_GROW_MIN  (64UL * 1024UL)
#define LIBC_UNISTD_SPIN_BEFORE_YIELD 256U

typedef struct libc_fd_slot
{
    bool used;
    int kernel_fd;
} libc_fd_slot_t;

static libc_fd_slot_t LibC_fd_slots[LIBC_FD_SLOTS];
static uintptr_t LibC_brk_base = 0;
static uintptr_t LibC_brk_curr = 0;
static uintptr_t LibC_brk_mapped_end = 0;
static volatile uint8_t LibC_unistd_lock = 0;
static volatile uint8_t LibC_stdio_null_mask = 0;
static const uint8_t LibC_stdio_null_stdin_bit = 1U << 0;
static const uint8_t LibC_stdio_null_stdout_bit = 1U << 1;
static const uint8_t LibC_stdio_null_stderr_bit = 1U << 2;

static void unistd_lock(void)
{
    unsigned int spins = 0U;
    while (__atomic_test_and_set(&LibC_unistd_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_unistd_lock, __ATOMIC_RELAXED) != 0U)
        {
            spins++;
            if (spins >= LIBC_UNISTD_SPIN_BEFORE_YIELD)
            {
                spins = 0U;
                (void) sys_yield();
            }
        }
    }
}

static void unistd_unlock(void)
{
    __atomic_clear(&LibC_unistd_lock, __ATOMIC_RELEASE);
}

static bool unistd_stdio_null_enabled(int fd)
{
    uint8_t mask = __atomic_load_n(&LibC_stdio_null_mask, __ATOMIC_ACQUIRE);
    if (fd == STDIN_FILENO)
        return (mask & LibC_stdio_null_stdin_bit) != 0U;
    if (fd == STDOUT_FILENO)
        return (mask & LibC_stdio_null_stdout_bit) != 0U;
    if (fd == STDERR_FILENO)
        return (mask & LibC_stdio_null_stderr_bit) != 0U;
    return false;
}

static void unistd_stdio_set_null_all(bool enabled)
{
    uint8_t value = enabled
                        ? (uint8_t) (LibC_stdio_null_stdin_bit |
                                     LibC_stdio_null_stdout_bit |
                                     LibC_stdio_null_stderr_bit)
                        : 0U;
    __atomic_store_n(&LibC_stdio_null_mask, value, __ATOMIC_RELEASE);
}

static uintptr_t unistd_align_up_uintptr(uintptr_t value, uintptr_t align)
{
    if (align == 0)
        return value;
    return (value + (align - 1U)) & ~(align - 1U);
}

static int unistd_brk_grow_to(uintptr_t target_end)
{
    if (target_end <= LibC_brk_mapped_end)
        return 0;

    if (LibC_brk_mapped_end == 0)
    {
        errno = ENOMEM;
        return -1;
    }

    uintptr_t needed = target_end - LibC_brk_mapped_end;
    uintptr_t grow = (needed > LIBC_BRK_GROW_MIN) ? needed : LIBC_BRK_GROW_MIN;
    grow = unistd_align_up_uintptr(grow, LIBC_BRK_PAGE_SIZE);
    if (grow == 0 || LibC_brk_mapped_end > UINTPTR_MAX - grow)
    {
        errno = ENOMEM;
        return -1;
    }

    void* map_rc = mmap((void*) LibC_brk_mapped_end,
                        (size_t) grow,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1,
                        0);
    if (map_rc == MAP_FAILED || (uintptr_t) map_rc != LibC_brk_mapped_end)
    {
        if (map_rc != MAP_FAILED && (uintptr_t) map_rc != LibC_brk_mapped_end)
            (void) munmap(map_rc, (size_t) grow);
        errno = ENOMEM;
        return -1;
    }

    LibC_brk_mapped_end += grow;
    return 0;
}

static int unistd_brk_ensure_initialized(void)
{
    if (LibC_brk_base != 0)
        return 0;

    uintptr_t init_size = unistd_align_up_uintptr(LIBC_BRK_GROW_MIN, LIBC_BRK_PAGE_SIZE);
    if (init_size == 0)
    {
        errno = ENOMEM;
        return -1;
    }

    void* base = mmap(NULL, (size_t) init_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
    {
        errno = ENOMEM;
        return -1;
    }

    LibC_brk_base = (uintptr_t) base;
    LibC_brk_curr = LibC_brk_base;
    LibC_brk_mapped_end = LibC_brk_base + init_size;
    return 0;
}

int libc_fd_adopt_kernel(int kernel_fd)
{
    int fd = -1;

    unistd_lock();
    for (size_t i = 0; i < LIBC_FD_SLOTS; i++)
    {
        if (LibC_fd_slots[i].used)
            continue;

        LibC_fd_slots[i].used = true;
        LibC_fd_slots[i].kernel_fd = kernel_fd;
        fd = (int) i + LIBC_FD_BASE;
        break;
    }
    unistd_unlock();

    return fd;
}

int libc_fd_get_kernel(int fd)
{
    int kernel_fd = -1;

    unistd_lock();
    int slot = fd - LIBC_FD_BASE;
    if (slot >= 0 && (size_t) slot < LIBC_FD_SLOTS &&
        LibC_fd_slots[(size_t) slot].used)
    {
        kernel_fd = LibC_fd_slots[(size_t) slot].kernel_fd;
    }
    unistd_unlock();

    return kernel_fd;
}

static bool unistd_fd_release_slot(int fd, int* out_kernel_fd)
{
    bool released = false;

    unistd_lock();
    int slot = fd - LIBC_FD_BASE;
    if (slot >= 0 && (size_t) slot < LIBC_FD_SLOTS &&
        LibC_fd_slots[(size_t) slot].used)
    {
        if (out_kernel_fd)
            *out_kernel_fd = LibC_fd_slots[(size_t) slot].kernel_fd;
        LibC_fd_slots[(size_t) slot].used = false;
        LibC_fd_slots[(size_t) slot].kernel_fd = -1;
        released = true;
    }
    unistd_unlock();

    return released;
}

static bool unistd_exec_has_slash(const char* file)
{
    if (!file)
        return false;

    while (*file != '\0')
    {
        if (*file == '/')
            return true;
        file++;
    }

    return false;
}

static int unistd_exec_collect_args(const char* first_arg, va_list ap, char* argv[], size_t argv_cap)
{
    size_t argc = 0U;
    const char* cursor = first_arg;

    while (cursor)
    {
        if (argc + 1U >= argv_cap)
        {
            errno = E2BIG;
            return -1;
        }

        argv[argc++] = (char*) cursor;
        cursor = va_arg(ap, const char*);
    }

    argv[argc] = NULL;
    return 0;
}

static bool unistd_path_has_prefix(const char* path, const char* prefix)
{
    if (!path || !prefix)
        return false;

    size_t i = 0;
    while (prefix[i] != '\0')
    {
        if (path[i] != prefix[i])
            return false;
        i++;
    }

    return true;
}

static ino_t unistd_inode_from_path(const char* path)
{
    // 64-bit FNV-1a hash, stable for a given path string.
    uint64_t h = 1469598103934665603ULL;
    if (!path)
        return (ino_t) 0;

    for (size_t i = 0; path[i] != '\0'; i++)
    {
        h ^= (uint8_t) path[i];
        h *= 1099511628211ULL;
    }

    if (h == 0)
        h = 1;
    return (ino_t) h;
}

int pipe(int pipefd[2])
{
    if (!pipefd)
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fds[2] = { -1, -1 };
    if (sys_pipe(kernel_fds) < 0)
    {
        errno = EMFILE;
        return -1;
    }

    int read_fd = libc_fd_adopt_kernel(kernel_fds[0]);
    if (read_fd < 0)
    {
        (void) sys_close(kernel_fds[0]);
        (void) sys_close(kernel_fds[1]);
        errno = EMFILE;
        return -1;
    }

    int write_fd = libc_fd_adopt_kernel(kernel_fds[1]);
    if (write_fd < 0)
    {
        (void) close(read_fd);
        (void) sys_close(kernel_fds[1]);
        errno = EMFILE;
        return -1;
    }

    pipefd[0] = read_fd;
    pipefd[1] = write_fd;
    return 0;
}

ssize_t read(int fd, void* buf, size_t count)
{
    if (count == 0U)
        return 0;

    if (!buf)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd == STDIN_FILENO)
    {
        if (unistd_stdio_null_enabled(STDIN_FILENO))
            return 0;

        uint8_t* out = (uint8_t*) buf;
        size_t total = 0U;
        while (total < count)
        {
            int ch = getchar();
            if (ch == EOF)
            {
                if (total == 0U)
                {
                    errno = EIO;
                    return -1;
                }
                break;
            }

            if (ch < 0 || ch > 255)
                continue;

            out[total++] = (uint8_t) ch;
            break;
        }

        return (ssize_t) total;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        errno = EBADF;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(fd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int rc = sys_read(kernel_fd, buf, count);
    if (rc < 0)
    {
        errno = (rc == -2) ? EAGAIN : EIO;
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
        if (unistd_stdio_null_enabled(fd))
            return (ssize_t) count;

        int rc = sys_console_write(buf, count);
        if (rc < 0)
        {
            errno = EIO;
            return -1;
        }
        return (ssize_t) rc;
    }
    if (fd == STDIN_FILENO)
    {
        errno = EBADF;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(fd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int rc = sys_write(kernel_fd, buf, count);
    if (rc < 0)
    {
        errno = (rc == -2) ? EAGAIN : EIO;
        return -1;
    }

    return (ssize_t) rc;
}

int open(const char* path, int flags, ...)
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    int unsupported = flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_LOCK | O_NONBLOCK);
    if (unsupported != 0)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t sys_flags = 0;
    int access_mode = flags & O_ACCMODE;
    switch (access_mode)
    {
        case O_RDONLY:
            sys_flags |= SYS_OPEN_READ;
            break;
        case O_WRONLY:
            sys_flags |= SYS_OPEN_WRITE;
            break;
        case O_RDWR:
            sys_flags |= SYS_OPEN_READ | SYS_OPEN_WRITE;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    if ((flags & O_CREAT) != 0)
    {
        sys_flags |= SYS_OPEN_CREATE;
    }

    if ((flags & O_LOCK) != 0)
    {
        sys_flags |= SYS_OPEN_LOCK;
    }

    if ((flags & O_TRUNC) != 0)
        sys_flags |= SYS_OPEN_TRUNC;

    int kernel_fd = sys_open(path, sys_flags);
    if (kernel_fd < 0)
    {
        errno = ((flags & O_CREAT) != 0) ? EIO : ENOENT;
        return -1;
    }

    int fd = libc_fd_adopt_kernel(kernel_fd);
    if (fd < 0)
    {
        (void) sys_close(kernel_fd);
        errno = EMFILE;
        return -1;
    }

    if ((flags & O_NONBLOCK) != 0)
    {
        int non_blocking = 1;
        if (ioctl(fd, FIONBIO, &non_blocking) < 0)
        {
            if (errno == ENOTTY)
                return fd;

            int saved_errno = errno;
            (void) close(fd);
            errno = saved_errno;
            return -1;
        }
    }

    return fd;
}

int close(int fd)
{
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
        return 0;

    int kernel_fd = -1;
    if (!unistd_fd_release_slot(fd, &kernel_fd))
    {
        errno = EBADF;
        return -1;
    }

    int rc = sys_close(kernel_fd);
    if (rc < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

off_t lseek(int fd, off_t offset, int whence)
{
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
    {
        errno = EINVAL;
        return (off_t) -1;
    }

    int kernel_fd = libc_fd_get_kernel(fd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return (off_t) -1;
    }

    int64_t rc = sys_lseek(kernel_fd, (int64_t) offset, whence);
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

int clear_screen(void)
{
    static const char clear_seq[] = "\x1b[2J\x1b[H";
    size_t written = 0U;

    if (isatty(STDOUT_FILENO) != 1)
    {
        errno = ENOTTY;
        return -1;
    }

    while (written < (sizeof(clear_seq) - 1U))
    {
        ssize_t rc = write(STDOUT_FILENO, clear_seq + written, (sizeof(clear_seq) - 1U) - written);
        if (rc < 0)
            return -1;
        if (rc == 0)
        {
            errno = EIO;
            return -1;
        }
        written += (size_t) rc;
    }

    return 0;
}

int brk(void* addr)
{
    if (!addr)
    {
        errno = EINVAL;
        return -1;
    }

    unistd_lock();
    if (unistd_brk_ensure_initialized() < 0)
    {
        unistd_unlock();
        return -1;
    }

    uintptr_t requested = (uintptr_t) addr;
    if (requested < LibC_brk_base)
    {
        unistd_unlock();
        errno = EINVAL;
        return -1;
    }

    if (requested > LibC_brk_mapped_end && unistd_brk_grow_to(requested) < 0)
    {
        unistd_unlock();
        return -1;
    }

    LibC_brk_curr = requested;
    unistd_unlock();
    return 0;
}

void* sbrk(intptr_t increment)
{
    unistd_lock();
    if (unistd_brk_ensure_initialized() < 0)
    {
        unistd_unlock();
        return (void*) -1;
    }

    uintptr_t old_break = LibC_brk_curr;
    if (increment == 0)
    {
        unistd_unlock();
        return (void*) old_break;
    }

    uintptr_t new_break = old_break;
    if (increment > 0)
    {
        uintptr_t delta = (uintptr_t) increment;
        if (old_break > UINTPTR_MAX - delta)
        {
            unistd_unlock();
            errno = ENOMEM;
            return (void*) -1;
        }
        new_break = old_break + delta;
        if (new_break > LibC_brk_mapped_end && unistd_brk_grow_to(new_break) < 0)
        {
            unistd_unlock();
            return (void*) -1;
        }
    }
    else
    {
        uintptr_t delta = (uintptr_t) (-(increment + 1)) + 1U;
        if (delta > (old_break - LibC_brk_base))
        {
            unistd_unlock();
            errno = ENOMEM;
            return (void*) -1;
        }
        new_break = old_break - delta;
    }

    LibC_brk_curr = new_break;
    unistd_unlock();
    return (void*) old_break;
}

int access(const char* path, int mode)
{
    if ((mode & ~(F_OK | R_OK | W_OK | X_OK)) != 0)
    {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0)
        return -1;

    if (mode == F_OK)
        return 0;

    if ((mode & R_OK) != 0 && (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0)
    {
        errno = EACCES;
        return -1;
    }

    if ((mode & W_OK) != 0 && (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0)
    {
        errno = EACCES;
        return -1;
    }

    if ((mode & X_OK) != 0 && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
    {
        errno = EACCES;
        return -1;
    }

    return 0;
}

pid_t fork(void)
{
    int rc = sys_fork();
    if (rc < 0)
    {
        errno = EAGAIN;
        return -1;
    }

    return (pid_t) rc;
}

int execve(const char* path, char* const argv[], char* const envp[])
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    int rc = sys_execve(path, (const char* const*) argv, (const char* const*) envp);
    if (rc < 0)
    {
        errno = ENOENT;
        return -1;
    }

    return rc;
}

int execv(const char* path, char* const argv[])
{
    return execve(path, argv, NULL);
}

int execvp(const char* file, char* const argv[])
{
    if (!file || file[0] == '\0')
    {
        errno = ENOENT;
        return -1;
    }

    if (unistd_exec_has_slash(file))
        return execve(file, argv, NULL);

    int rc = execve(file, argv, NULL);
    if (rc == 0)
        return 0;

    char candidate[LIBC_EXEC_PATH_MAX];
    int len = snprintf(candidate, sizeof(candidate), "/bin/%s", file);
    if (len <= 0 || (size_t) len >= sizeof(candidate))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    return execve(candidate, argv, NULL);
}

int execl(const char* path, const char* arg, ...)
{
    char* argv[LIBC_EXEC_ARG_MAX];
    va_list ap;
    va_start(ap, arg);
    int collect_rc = unistd_exec_collect_args(arg, ap, argv, LIBC_EXEC_ARG_MAX);
    va_end(ap);
    if (collect_rc < 0)
        return -1;

    return execve(path, argv, NULL);
}

int execlp(const char* file, const char* arg, ...)
{
    char* argv[LIBC_EXEC_ARG_MAX];
    va_list ap;
    va_start(ap, arg);
    int collect_rc = unistd_exec_collect_args(arg, ap, argv, LIBC_EXEC_ARG_MAX);
    va_end(ap);
    if (collect_rc < 0)
        return -1;

    return execvp(file, argv);
}

int kill(pid_t pid, int sig)
{
    if (pid <= 0)
    {
        errno = ESRCH;
        return -1;
    }
    if (sig < 0 || sig >= NSIG)
    {
        errno = EINVAL;
        return -1;
    }

    int rc = sys_kill((int) pid, sig);
    if (rc < 0)
    {
        errno = ESRCH;
        return -1;
    }

    return 0;
}

int daemon(int nochdir, int noclose)
{
    int first_fork = fork();
    if (first_fork < 0)
        return -1;
    if (first_fork > 0)
        _exit(0);

    int second_fork = fork();
    if (second_fork < 0)
        return -1;
    if (second_fork > 0)
        _exit(0);

    if (!nochdir && fs_is_dir("/") != 1)
    {
        errno = ENOENT;
        return -1;
    }

    if (!noclose)
        unistd_stdio_set_null_all(true);

    return 0;
}

__attribute__((__noreturn__)) void _exit(int status)
{
    sys_exit(status);
}

int waitpid(pid_t pid, int* status, int options)
{
    if ((options & ~WNOHANG) != 0)
    {
        errno = EINVAL;
        return -1;
    }

    for (;;)
    {
        int raw_status = 0;
        int raw_signal = 0;
        int rc = sys_waitpid((int) pid, &raw_status, &raw_signal);
        if (rc < 0)
        {
            errno = ECHILD;
            return -1;
        }

        if (rc == 0)
        {
            if ((options & WNOHANG) != 0)
            {
                if (status)
                    *status = 0;
                return 0;
            }

            (void) sys_yield();
            continue;
        }

        int packed = 0;
        if (raw_signal != 0)
            packed = raw_signal & 0x7F;
        else
            packed = (raw_status & 0xFF) << 8;

        if (status)
            *status = packed;

        return rc;
    }
}

int stat(const char* path, struct stat* out_stat)
{
    if (!path || path[0] == '\0' || !out_stat)
    {
        errno = EINVAL;
        return -1;
    }

    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->st_dev = (dev_t) 1;
    out_stat->st_ino = unistd_inode_from_path(path);
    out_stat->st_uid = (uid_t) 0;
    out_stat->st_gid = (gid_t) 0;
    out_stat->st_rdev = (dev_t) 0;
    out_stat->st_blksize = LIBC_STAT_BLKSIZE;
    out_stat->st_atime = (time_t) 0;
    out_stat->st_mtime = (time_t) 0;
    out_stat->st_ctime = (time_t) 0;

    if (fs_is_dir(path) == 1)
    {
        out_stat->st_mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
        out_stat->st_nlink = 2;
        out_stat->st_size = 0;
        out_stat->st_blocks = 0;
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        if (errno == 0)
            errno = ENOENT;
        return -1;
    }

    off_t end = lseek(fd, 0, SEEK_END);
    if (close(fd) < 0)
    {
        errno = EIO;
        return -1;
    }
    if (end < 0)
        end = 0;

    bool executable = unistd_path_has_prefix(path, "/bin/");
    mode_t perm = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    if (executable)
        perm |= S_IXUSR | S_IXGRP | S_IXOTH;

    out_stat->st_mode = S_IFREG | perm;
    out_stat->st_nlink = 1;
    out_stat->st_size = end;
    out_stat->st_blocks = (blkcnt_t) (((end > 0) ? ((end + LIBC_STAT_BLKSIZE - 1) / LIBC_STAT_BLKSIZE) : 0));
    return 0;
}

int fstat(int fd, struct stat* out_stat)
{
    if (!out_stat)
    {
        errno = EINVAL;
        return -1;
    }

    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->st_dev = (dev_t) 1;
    out_stat->st_uid = (uid_t) 0;
    out_stat->st_gid = (gid_t) 0;
    out_stat->st_rdev = (dev_t) 0;
    out_stat->st_blksize = LIBC_STAT_BLKSIZE;
    out_stat->st_atime = (time_t) 0;
    out_stat->st_mtime = (time_t) 0;
    out_stat->st_ctime = (time_t) 0;

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        out_stat->st_ino = (ino_t) (fd + 1);
        out_stat->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        out_stat->st_nlink = 1;
        out_stat->st_size = 0;
        out_stat->st_blocks = 0;
        return 0;
    }

    int kernel_fd = libc_fd_get_kernel(fd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int64_t current = sys_lseek(kernel_fd, 0, SEEK_CUR);
    if (current < 0)
    {
        errno = EINVAL;
        return -1;
    }

    int64_t end = sys_lseek(kernel_fd, 0, SEEK_END);
    if (end < 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (sys_lseek(kernel_fd, current, SEEK_SET) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    out_stat->st_ino = (ino_t) (kernel_fd + 1);
    out_stat->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    out_stat->st_nlink = 1;
    out_stat->st_size = (off_t) ((end > 0) ? end : 0);
    out_stat->st_blocks = (blkcnt_t) ((out_stat->st_size > 0)
                                          ? ((out_stat->st_size + LIBC_STAT_BLKSIZE - 1) / LIBC_STAT_BLKSIZE)
                                          : 0);
    return 0;
}

int mkdir(const char* path, mode_t mode)
{
    (void) mode;

    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    if (fs_mkdir(path) < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    if (len == 0U)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    int supported_flags = MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS;
    bool is_private = (flags & MAP_PRIVATE) != 0;
    bool is_shared = (flags & MAP_SHARED) != 0;
    if ((flags & ~supported_flags) != 0)
    {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    if (is_private == is_shared)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    bool is_anonymous = (flags & MAP_ANONYMOUS) != 0;

    if (addr && (((uintptr_t) addr & (LIBC_BRK_PAGE_SIZE - 1U)) != 0U))
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (offset < 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (((uint64_t) offset & (LIBC_BRK_PAGE_SIZE - 1U)) != 0U)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    uint64_t sys_prot = 0;
    int supported_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    if ((prot & ~supported_prot) != 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((prot & PROT_READ) != 0)
        sys_prot |= SYS_PROT_READ;
    if ((prot & PROT_WRITE) != 0)
        sys_prot |= SYS_PROT_WRITE;
    if ((prot & PROT_EXEC) != 0)
        sys_prot |= SYS_PROT_EXEC;

    int kernel_fd = -1;
    if (is_anonymous)
    {
        if (fd != -1 || offset != 0)
        {
            errno = EINVAL;
            return MAP_FAILED;
        }
    }
    else
    {
        if (fd < 0)
        {
            errno = EBADF;
            return MAP_FAILED;
        }

        kernel_fd = libc_fd_get_kernel(fd);
        if (kernel_fd < 0)
        {
            errno = EBADF;
            return MAP_FAILED;
        }
    }

    void* rc = sys_map_ex(addr,
                          len,
                          sys_prot,
                          (uint64_t) flags,
                          kernel_fd,
                          (uint64_t) offset);
    if (!rc)
    {
        errno = is_anonymous ? ENOMEM : EINVAL;
        return MAP_FAILED;
    }

    return rc;
}

int munmap(void* addr, size_t len)
{
    if (!addr || len == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (((uintptr_t) addr & (LIBC_BRK_PAGE_SIZE - 1U)) != 0U)
    {
        errno = EINVAL;
        return -1;
    }

    if (sys_unmap(addr, len) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int mprotect(void* addr, size_t len, int prot)
{
    if (!addr || len == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (((uintptr_t) addr & (LIBC_BRK_PAGE_SIZE - 1U)) != 0U)
    {
        errno = EINVAL;
        return -1;
    }

    uint64_t sys_prot = 0;
    int supported_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    if ((prot & ~supported_prot) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    if ((prot & PROT_READ) != 0)
        sys_prot |= SYS_PROT_READ;
    if ((prot & PROT_WRITE) != 0)
        sys_prot |= SYS_PROT_WRITE;
    if ((prot & PROT_EXEC) != 0)
        sys_prot |= SYS_PROT_EXEC;

    if (sys_mprotect(addr, len, sys_prot) < 0)
    {
        errno = EACCES;
        return -1;
    }

    return 0;
}

int sched_yield(void)
{
    if (sys_yield() < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
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
    if (usec == 0U)
        return 0;

    /* Avant : (usec + 999) / 1000 forçait 1..999 µs → sys_sleep_ms(1) (~1 ms).
     * Les boucles courtes (poll, retry socket) perdaient un ordre de grandeur. */
    if (usec < 1000U)
    {
        if (sched_yield() < 0)
        {
            errno = EIO;
            return -1;
        }
        return 0;
    }

    uint32_t ms = (uint32_t) (((uint64_t) usec + 999ULL) / 1000ULL);
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
