#ifndef _NET_IF_ARP_H
#define _NET_IF_ARP_H

#include <sys/socket.h>
#include <UAPI/Net.h>

#ifdef __cplusplus
extern "C" {
#endif

struct arpreq
{
    struct sockaddr arp_pa;
    struct sockaddr arp_ha;
    int arp_flags;
    char arp_dev[IFNAMSIZ];
};

#ifdef __cplusplus
}
#endif

#endif
