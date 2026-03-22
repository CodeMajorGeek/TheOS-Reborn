#ifndef _SOCKET_NET_PRIVATE_H
#define _SOCKET_NET_PRIVATE_H

#include <Network/Socket.h>

typedef struct net_socket_ipv4_header
{
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_len_be;
    uint16_t identification_be;
    uint16_t flags_frag_be;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum_be;
    uint32_t src_addr_be;
    uint32_t dst_addr_be;
} __attribute__((packed)) net_socket_ipv4_header_t;

typedef struct net_socket_udp_header
{
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint16_t len_be;
    uint16_t checksum_be;
} __attribute__((packed)) net_socket_udp_header_t;

typedef struct net_socket_wait_context
{
    const net_socket_udp_entry_t* entry;
} net_socket_wait_context_t;

static inline uint16_t NET_socket_read_be16(const uint8_t* bytes);
static inline uint32_t NET_socket_read_be32(const uint8_t* bytes);
static inline void NET_socket_write_be16(uint8_t* bytes, uint16_t value);
static inline void NET_socket_write_be32(uint8_t* bytes, uint32_t value);
static inline bool NET_socket_is_loopback_ipv4(uint32_t addr_be);

static uint16_t NET_socket_checksum_finalize(uint32_t sum);
static uint16_t NET_socket_checksum_bytes(const uint8_t* data, size_t len);
static uint16_t NET_socket_udp_checksum(uint32_t src_addr_be,
                                        uint32_t dst_addr_be,
                                        const uint8_t* udp_segment,
                                        size_t udp_len);
static bool NET_socket_port_conflict_locked(uint32_t exclude_socket_id, uint32_t local_addr_be, uint16_t local_port);
static uint16_t NET_socket_alloc_ephemeral_port_locked(uint32_t local_addr_be);
static bool NET_socket_auto_bind_locked(net_socket_udp_entry_t* entry);
static uint32_t NET_socket_enqueue_udp_locked(uint32_t dst_addr_be,
                                              uint16_t dst_port,
                                              uint32_t src_addr_be,
                                              uint16_t src_port,
                                              const uint8_t* payload,
                                              size_t payload_len);
static bool NET_socket_wait_rx_empty_predicate(void* context);
static bool NET_socket_get_udp_entry_locked(uint32_t owner_pid, uint32_t socket_id, net_socket_udp_entry_t** out_entry);

#endif
