#ifndef _UAPI_NET_H
#define _UAPI_NET_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * Linux-compatible generic ioctl requests used by raw network endpoints.
 */
#ifndef FIONREAD
#define FIONREAD 0x541BUL
#endif

#ifndef FIONBIO
#define FIONBIO 0x5421UL
#endif

/*
 * Linux-compatible private network ioctl base.
 */
#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE 0x89F0UL
#endif

#ifndef SIOCDARP
#define SIOCDARP 0x8953UL
#endif

#ifndef SIOCGARP
#define SIOCGARP 0x8954UL
#endif

#ifndef SIOCSARP
#define SIOCSARP 0x8955UL
#endif

#ifndef IFNAMSIZ
#define IFNAMSIZ 16U
#endif

#ifndef AF_UNSPEC
#define AF_UNSPEC 0U
#endif

#ifndef AF_INET
#define AF_INET 2U
#endif

#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1U
#endif

#ifndef ARPOP_REQUEST
#define ARPOP_REQUEST 1U
#endif

#ifndef ARPOP_REPLY
#define ARPOP_REPLY 2U
#endif

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800U
#endif

#ifndef ETH_P_ARP
#define ETH_P_ARP 0x0806U
#endif

#ifndef ATF_COM
#define ATF_COM 0x02U
#endif

#ifndef ATF_PERM
#define ATF_PERM 0x04U
#endif

#ifndef ATF_PUBL
#define ATF_PUBL 0x08U
#endif

#ifndef ATF_USETRAILERS
#define ATF_USETRAILERS 0x10U
#endif

#ifndef ATF_NETMASK
#define ATF_NETMASK 0x20U
#endif

#ifndef ATF_DONTPUB
#define ATF_DONTPUB 0x40U
#endif

#ifndef ATF_MAGIC
#define ATF_MAGIC 0x80U
#endif

#define NET_RAW_IOCTL_GET_STATS (SIOCDEVPRIVATE + 0UL)

#ifndef __ASSEMBLER__
typedef struct sys_sockaddr
{
    uint16_t sa_family;
    uint8_t sa_data[14];
} sys_sockaddr_t;

typedef struct sys_arpreq
{
    sys_sockaddr_t arp_pa;
    sys_sockaddr_t arp_ha;
    int32_t arp_flags;
    uint8_t arp_dev[IFNAMSIZ];
} sys_arpreq_t;

typedef struct sys_net_raw_stats
{
    uint64_t rx_packets;
    uint64_t rx_dropped;
    uint64_t tx_packets;
    uint64_t tx_dropped;
    uint64_t irq_count;
    uint32_t rx_queue_depth;
    uint32_t rx_queue_capacity;
    uint8_t link_up;
    uint8_t irq_mode;
    uint8_t reserved[2];
    uint8_t mac[6];
    uint8_t reserved2[2];
} sys_net_raw_stats_t;
#endif

#endif
