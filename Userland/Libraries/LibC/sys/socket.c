#include <errno.h>
#include <libc_fd.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>

int socket(int domain, int type, int protocol)
{
    int kernel_fd = sys_socket(domain, type, protocol);
    if (kernel_fd < 0)
    {
        if (domain != AF_INET && domain != AF_UNIX)
            errno = EAFNOSUPPORT;
        else if (type != SOCK_DGRAM && type != SOCK_STREAM)
            errno = ENOTSUP;
        else
            errno = EIO;
        return -1;
    }

    int user_fd = libc_fd_adopt_kernel(kernel_fd);
    if (user_fd < 0)
    {
        (void) sys_close(kernel_fd);
        errno = EMFILE;
        return -1;
    }

    return user_fd;
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr))
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (sys_bind(kernel_fd, addr, (size_t) addrlen) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int listen(int sockfd, int backlog)
{
    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (sys_listen(kernel_fd, backlog) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen)
{
    if ((addr == NULL) != (addrlen == NULL))
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int accepted_kernel_fd = sys_accept(kernel_fd, addr, addrlen);
    if (accepted_kernel_fd == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (accepted_kernel_fd < 0)
    {
        errno = EIO;
        return -1;
    }

    int user_fd = libc_fd_adopt_kernel(accepted_kernel_fd);
    if (user_fd < 0)
    {
        (void) sys_close(accepted_kernel_fd);
        errno = EMFILE;
        return -1;
    }

    return user_fd;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr))
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int rc = sys_connect(kernel_fd, addr, (size_t) addrlen);
    if (rc == -2)
    {
        errno = EINPROGRESS;
        return -1;
    }
    if (rc < 0)
    {
        if (addr->sa_family == AF_UNSPEC)
            errno = ENOTCONN;
        else if (addr->sa_family != AF_INET && addr->sa_family != AF_UNIX)
            errno = EAFNOSUPPORT;
        else
            errno = EINVAL;
        return -1;
    }

    return 0;
}

int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen)
{
    if (!addr || !addrlen)
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (sys_getsockname(kernel_fd, addr, addrlen) < 0)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen)
{
    if (!addr || !addrlen)
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (sys_getpeername(kernel_fd, addr, addrlen) < 0)
    {
        errno = ENOTCONN;
        return -1;
    }

    return 0;
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags)
{
    if (len != 0U && !buf)
    {
        errno = EINVAL;
        return -1;
    }
    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    int rc = sys_sendto(kernel_fd, buf, len, flags, NULL, 0);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = ENOTCONN;
        return -1;
    }
    return (ssize_t) rc;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen)
{
    if ((len != 0U && !buf) || ((dest_addr == NULL) != (addrlen == 0U)))
    {
        errno = EINVAL;
        return -1;
    }

    if (dest_addr && addrlen < sizeof(struct sockaddr))
    {
        errno = EINVAL;
        return -1;
    }

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    if (dest_addr && dest_addr->sa_family != AF_INET && dest_addr->sa_family != AF_UNIX)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }

    int rc = sys_sendto(kernel_fd, buf, len, flags, dest_addr, (size_t) addrlen);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = dest_addr ? EIO : ENOTCONN;
        return -1;
    }

    return (ssize_t) rc;
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags)
{
    if (len != 0U && !buf)
    {
        errno = EINVAL;
        return -1;
    }
    if (len == 0U)
        return 0;
    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    int rc = sys_recvfrom(kernel_fd, buf, len, flags, NULL, NULL);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = ECONNRESET;
        return -1;
    }
    return (ssize_t) rc;
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen)
{
    if ((len != 0U && !buf) || ((src_addr == NULL) != (addrlen == NULL)))
    {
        errno = EINVAL;
        return -1;
    }

    if (len == 0U)
        return 0;

    int kernel_fd = libc_fd_get_kernel(sockfd);
    if (kernel_fd < 0)
    {
        errno = EBADF;
        return -1;
    }

    int rc = sys_recvfrom(kernel_fd, buf, len, flags, src_addr, addrlen);
    if (rc == -2)
    {
        errno = EAGAIN;
        return -1;
    }
    if (rc < 0)
    {
        errno = EIO;
        return -1;
    }

    return (ssize_t) rc;
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen)
{
    (void) sockfd;

    if (level == SOL_SOCKET && optname == SO_ERROR && optval && optlen && *optlen >= (socklen_t) sizeof(int))
    {
        *(int*) optval = 0;
        *optlen = (socklen_t) sizeof(int);
        return 0;
    }

    errno = ENOTSUP;
    return -1;
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen)
{
    (void) sockfd;
    (void) optval;
    (void) optlen;

    if (level == SOL_SOCKET &&
        (optname == SO_BROADCAST || optname == SO_REUSEADDR || optname == SO_KEEPALIVE))
    {
        return 0;
    }

    errno = ENOTSUP;
    return -1;
}
