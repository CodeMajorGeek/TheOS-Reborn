#ifndef _THEDHCPD_PROTO_H
#define _THEDHCPD_PROTO_H

#include <stdint.h>

#include <UAPI/Net.h>

#define DHCPD_ETH_HEADER_LEN        14U
#define DHCPD_IPV4_MIN_HEADER_LEN   20U
#define DHCPD_UDP_HEADER_LEN        8U
#define DHCPD_BOOTP_FIXED_LEN       236U
#define DHCPD_DHCP_MAGIC_COOKIE     0x63825363UL
#define DHCPD_CLIENT_PORT           68U
#define DHCPD_SERVER_PORT           67U
#define DHCPD_ETH_BROADCAST_BYTE    0xFFU

#define DHCPD_IP_PROTO_UDP          17U
#define DHCPD_IP_VERSION_IHL        0x45U
#define DHCPD_IP_TTL_DEFAULT        64U
#define DHCPD_IP_FLAGS_DF           0x4000U

#define DHCPD_BOOTP_OP_REQUEST      1U
#define DHCPD_BOOTP_OP_REPLY        2U
#define DHCPD_BOOTP_HTYPE_ETHERNET  1U
#define DHCPD_BOOTP_HLEN_ETHERNET   6U
#define DHCPD_BOOTP_FLAGS_BROADCAST 0x8000U

#define DHCPD_OPT_PAD               0U
#define DHCPD_OPT_SUBNET_MASK       1U
#define DHCPD_OPT_ROUTER            3U
#define DHCPD_OPT_DNS               6U
#define DHCPD_OPT_HOSTNAME          12U
#define DHCPD_OPT_DOMAIN            15U
#define DHCPD_OPT_REQ_IP            50U
#define DHCPD_OPT_MSG_TYPE          53U
#define DHCPD_OPT_SERVER_ID         54U
#define DHCPD_OPT_PARAM_REQ_LIST    55U
#define DHCPD_OPT_CLIENT_ID         61U
#define DHCPD_OPT_END               255U

#define DHCPD_MSG_DISCOVER          1U
#define DHCPD_MSG_OFFER             2U
#define DHCPD_MSG_REQUEST           3U
#define DHCPD_MSG_DECLINE           4U
#define DHCPD_MSG_ACK               5U
#define DHCPD_MSG_NAK               6U
#define DHCPD_MSG_RELEASE           7U
#define DHCPD_MSG_INFORM            8U

#define DHCPD_DISCOVER_PERIOD_MS    5000U
#define DHCPD_LOOP_SLEEP_MS         100U
#define DHCPD_DISCOVER_LOG_EVERY    6U
#define DHCPD_NET_RETRY_MS          2000U
#define DHCPD_TX_FRAME_MAX          1024U
#define DHCPD_RX_FRAME_MAX          2048U
#define DHCPD_LOG_LINE_MAX          256U
#define DHCPD_NET_NODE_PATH         "/dev/net0"

typedef struct dhcpd_eth_header
{
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype_be;
} __attribute__((packed)) dhcpd_eth_header_t;

typedef struct dhcpd_ipv4_header
{
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_len_be;
    uint16_t identification_be;
    uint16_t flags_frag_be;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum_be;
    uint32_t src_addr_be;
    uint32_t dst_addr_be;
} __attribute__((packed)) dhcpd_ipv4_header_t;

typedef struct dhcpd_udp_header
{
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint16_t length_be;
    uint16_t checksum_be;
} __attribute__((packed)) dhcpd_udp_header_t;

typedef struct dhcpd_bootp_header
{
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid_be;
    uint16_t secs_be;
    uint16_t flags_be;
    uint32_t ciaddr_be;
    uint32_t yiaddr_be;
    uint32_t siaddr_be;
    uint32_t giaddr_be;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
} __attribute__((packed)) dhcpd_bootp_header_t;

#endif
