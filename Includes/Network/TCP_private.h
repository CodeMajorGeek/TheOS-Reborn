#ifndef _TCP_NET_PRIVATE_H
#define _TCP_NET_PRIVATE_H

#include <Network/TCP.h>

typedef struct net_tcp_ipv4_header
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
} __attribute__((packed)) net_tcp_ipv4_header_t;

typedef struct net_tcp_header
{
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint32_t seq_be;
    uint32_t ack_be;
    uint8_t data_off_reserved;
    uint8_t flags;
    uint16_t window_be;
    uint16_t checksum_be;
    uint16_t urgent_ptr_be;
} __attribute__((packed)) net_tcp_header_t;

typedef enum net_tcp_wait_kind
{
    NET_TCP_WAIT_RX = 0,
    NET_TCP_WAIT_ACCEPT,
    NET_TCP_WAIT_CONNECT
} net_tcp_wait_kind_t;

typedef struct net_tcp_wait_context
{
    const net_tcp_entry_t* entry;
    net_tcp_wait_kind_t kind;
} net_tcp_wait_context_t;

typedef struct net_tcp_tx_plan
{
    bool valid;
    uint32_t src_addr_be;
    uint32_t dst_addr_be;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    const uint8_t* payload;
    size_t payload_len;
} net_tcp_tx_plan_t;

static inline uint16_t NET_tcp_read_be16(const uint8_t* bytes);
static inline uint32_t NET_tcp_read_be32(const uint8_t* bytes);
static inline void NET_tcp_write_be16(uint8_t* bytes, uint16_t value);
static inline void NET_tcp_write_be32(uint8_t* bytes, uint32_t value);
static inline bool NET_tcp_is_loopback_ipv4(uint32_t addr_be);

static uint16_t NET_tcp_checksum_finalize(uint32_t sum);
static uint16_t NET_tcp_checksum_bytes(const uint8_t* data, size_t len);
static uint16_t NET_tcp_segment_checksum(uint32_t src_addr_be, uint32_t dst_addr_be, const uint8_t* segment, size_t segment_len);
static bool NET_tcp_port_conflict_locked(uint32_t exclude_socket_id, uint32_t local_addr_be, uint16_t local_port);
static uint16_t NET_tcp_alloc_ephemeral_port_locked(uint32_t local_addr_be);
static bool NET_tcp_auto_bind_locked(net_tcp_entry_t* entry);
static bool NET_tcp_wait_predicate(void* context);
static bool NET_tcp_get_entry_locked(uint32_t owner_pid, uint32_t socket_id, net_tcp_entry_t** out_entry);
static int32_t NET_tcp_find_connection_index_locked(uint32_t src_addr_be,
                                                    uint32_t dst_addr_be,
                                                    uint16_t src_port,
                                                    uint16_t dst_port);
static int32_t NET_tcp_find_listener_index_locked(uint32_t dst_addr_be, uint16_t dst_port);
static int32_t NET_tcp_alloc_entry_index_locked(void);
static bool NET_tcp_emit_segment(uint32_t src_addr_be,
                                 uint32_t dst_addr_be,
                                 uint16_t src_port,
                                 uint16_t dst_port,
                                 uint32_t seq,
                                 uint32_t ack,
                                 uint8_t flags,
                                 const uint8_t* payload,
                                 size_t payload_len);
static bool NET_tcp_queue_accept_locked(net_tcp_entry_t* listener, uint32_t child_socket_id);
static void NET_tcp_process_segment(uint32_t src_addr_be,
                                    uint32_t dst_addr_be,
                                    uint16_t src_port,
                                    uint16_t dst_port,
                                    uint32_t seq,
                                    uint32_t ack,
                                    uint8_t flags,
                                    const uint8_t* payload,
                                    size_t payload_len);

#endif
