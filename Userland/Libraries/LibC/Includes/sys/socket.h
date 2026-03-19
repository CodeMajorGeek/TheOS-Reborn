#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SA_FAMILY_T_DECLARED
typedef unsigned short sa_family_t;
#define _SA_FAMILY_T_DECLARED 1
#endif

#ifndef _SOCKLEN_T_DECLARED
typedef unsigned int socklen_t;
#define _SOCKLEN_T_DECLARED 1
#endif

struct sockaddr
{
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage
{
    sa_family_t ss_family;
    char __ss_padding[126];
};

/* Address/protocol families (Linux-compatible values). */
#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif
#ifndef AF_UNIX
#define AF_UNIX   1
#endif
#ifndef AF_INET
#define AF_INET   2
#endif
#ifndef AF_INET6
#define AF_INET6  10
#endif

#ifndef PF_UNSPEC
#define PF_UNSPEC AF_UNSPEC
#endif
#ifndef PF_UNIX
#define PF_UNIX   AF_UNIX
#endif
#ifndef PF_INET
#define PF_INET   AF_INET
#endif
#ifndef PF_INET6
#define PF_INET6  AF_INET6
#endif

/* Socket types. */
#ifndef SOCK_STREAM
#define SOCK_STREAM    1
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM     2
#endif
#ifndef SOCK_RAW
#define SOCK_RAW       3
#endif
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif

/* Socket options/flags (minimal commonly used subset). */
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_ERROR
#define SO_ERROR 4
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST 6
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 21
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20
#endif

#ifndef MSG_PEEK
#define MSG_PEEK 0x2
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif

#ifndef SHUT_RD
#define SHUT_RD   0
#endif
#ifndef SHUT_WR
#define SHUT_WR   1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t send(int sockfd, const void* buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen);
ssize_t recv(int sockfd, void* buf, size_t len, int flags);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);

#ifdef __cplusplus
}
#endif

#endif
