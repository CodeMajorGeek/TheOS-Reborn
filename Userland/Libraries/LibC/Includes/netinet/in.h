#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _SA_FAMILY_T_DECLARED
typedef uint16_t sa_family_t;
#define _SA_FAMILY_T_DECLARED 1
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr
{
    in_addr_t s_addr;
};

struct sockaddr_in
{
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

#ifndef INADDR_ANY
#define INADDR_ANY       ((in_addr_t) 0x00000000U)
#endif
#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST ((in_addr_t) 0xFFFFFFFFU)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK  ((in_addr_t) 0x7F000001U)
#endif

#ifndef IPPROTO_IP
#define IPPROTO_IP   0
#endif
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP  6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP  17
#endif

#ifndef IPPORT_RESERVED
#define IPPORT_RESERVED     1024
#endif
#ifndef IPPORT_USERRESERVED
#define IPPORT_USERRESERVED 5000
#endif

#ifdef __cplusplus
}
#endif

#endif
