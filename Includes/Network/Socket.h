#ifndef _SOCKET_NET_H
#define _SOCKET_NET_H

#include <Debug/Spinlock.h>
#include <Task/Task.h>
#include <UAPI/Net.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_SOCKET_AF_INET               AF_INET
#define NET_SOCKET_SOCK_DGRAM            2U
#define NET_SOCKET_IPPROTO_UDP           17U

#define NET_SOCKET_MSG_DONTWAIT          0x40U

#define NET_SOCKET_MAX_UDP_SOCKETS       64U
#define NET_SOCKET_UDP_RX_QUEUE_LEN      64U
#define NET_SOCKET_UDP_MAX_PAYLOAD       1472U

#define NET_SOCKET_IPV4_ANY              0x00000000U
#define NET_SOCKET_IPV4_BROADCAST        0xFFFFFFFFU
#define NET_SOCKET_IPV4_LOOPBACK_BE      0x7F000001U
#define NET_SOCKET_IPV4_LOOPBACK_MASK_BE 0xFF000000U
#define NET_SOCKET_IPV4_LOOPBACK_NET_BE  0x7F000000U

#define NET_SOCKET_EPHEMERAL_PORT_MIN    49152U
#define NET_SOCKET_EPHEMERAL_PORT_MAX    65535U

#define NET_SOCKET_ETH_HEADER_LEN        14U
#define NET_SOCKET_IPV4_MIN_HEADER_LEN   20U
#define NET_SOCKET_UDP_HEADER_LEN        8U
#define NET_SOCKET_ETHERTYPE_IPV4        0x0800U
#define NET_SOCKET_IPV4_PROTO_UDP        17U
#define NET_SOCKET_IPV4_TTL_DEFAULT      64U
#define NET_SOCKET_IPV4_FLAGS_DF         0x4000U
#define NET_SOCKET_ETH_FRAME_MAX_BYTES   1518U

typedef struct net_socket_endpoint
{
    uint32_t ipv4_addr_be;
    uint16_t port;
} net_socket_endpoint_t;

typedef struct net_socket_udp_datagram
{
    uint16_t len;
    uint8_t payload[NET_SOCKET_UDP_MAX_PAYLOAD];
    net_socket_endpoint_t src;
    net_socket_endpoint_t dst;
} net_socket_udp_datagram_t;

typedef struct net_socket_udp_entry
{
    bool used;
    uint32_t owner_pid;
    bool bound;
    bool non_blocking;
    bool connected;
    bool waitq_ready;
    uint16_t local_port;
    uint32_t local_addr_be;
    uint16_t peer_port;
    uint32_t peer_addr_be;
    uint16_t rx_head;
    uint16_t rx_tail;
    uint32_t rx_count;
    task_wait_queue_t rx_waitq;
    net_socket_udp_datagram_t rx_queue[NET_SOCKET_UDP_RX_QUEUE_LEN];
} net_socket_udp_entry_t;

typedef struct net_socket_runtime_state
{
    bool lock_ready;
    bool initialized;
    spinlock_t lock;
    uint16_t next_ephemeral_port;
    uint16_t next_ipv4_identification;
    net_socket_udp_entry_t udp_entries[NET_SOCKET_MAX_UDP_SOCKETS];
} net_socket_runtime_state_t;

void NET_socket_init(void);
bool NET_socket_is_ready(void);
bool NET_socket_create_udp(uint32_t owner_pid, uint32_t* out_socket_id);
bool NET_socket_close_udp(uint32_t owner_pid, uint32_t socket_id);
bool NET_socket_set_non_blocking(uint32_t owner_pid, uint32_t socket_id, bool non_blocking, bool* out_non_blocking);
bool NET_socket_bind_udp(uint32_t owner_pid, uint32_t socket_id, uint32_t local_addr_be, uint16_t local_port);
bool NET_socket_connect_udp(uint32_t owner_pid, uint32_t socket_id, uint32_t peer_addr_be, uint16_t peer_port);
bool NET_socket_disconnect_udp(uint32_t owner_pid, uint32_t socket_id);
bool NET_socket_getsockname_udp(uint32_t owner_pid,
                                uint32_t socket_id,
                                uint32_t* out_local_addr_be,
                                uint16_t* out_local_port);
bool NET_socket_getpeername_udp(uint32_t owner_pid,
                                uint32_t socket_id,
                                bool* out_connected,
                                uint32_t* out_peer_addr_be,
                                uint16_t* out_peer_port);
bool NET_socket_sendto_udp(uint32_t owner_pid,
                           uint32_t socket_id,
                           uint32_t dst_addr_be,
                           uint16_t dst_port,
                           const uint8_t* payload,
                           size_t payload_len,
                           size_t* out_sent);
bool NET_socket_recvfrom_udp(uint32_t owner_pid,
                             uint32_t socket_id,
                             uint8_t* out_payload,
                             size_t out_cap,
                             size_t* out_len,
                             uint32_t* out_src_addr_be,
                             uint16_t* out_src_port,
                             bool force_non_blocking,
                             bool* out_would_block);
bool NET_socket_pending_udp_bytes(uint32_t owner_pid, uint32_t socket_id, size_t* out_bytes);
void NET_socket_on_ethernet_frame(const uint8_t* frame, size_t frame_len);

#endif
