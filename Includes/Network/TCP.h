#ifndef _TCP_NET_H
#define _TCP_NET_H

#include <Debug/Spinlock.h>
#include <Task/Task.h>
#include <UAPI/Net.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_TCP_AF_INET                    AF_INET
#define NET_TCP_SOCK_STREAM                1U
#define NET_TCP_IPPROTO_TCP                6U
#define NET_TCP_MSG_DONTWAIT               0x40U

#define NET_TCP_MAX_SOCKETS                64U
#define NET_TCP_RX_BUFFER_BYTES            8192U
#define NET_TCP_ACCEPT_QUEUE_LEN           16U
#define NET_TCP_MAX_PAYLOAD                1460U

#define NET_TCP_IPV4_ANY                   0x00000000U
#define NET_TCP_IPV4_LOOPBACK_BE           0x7F000001U
#define NET_TCP_IPV4_LOOPBACK_MASK_BE      0xFF000000U
#define NET_TCP_IPV4_LOOPBACK_NET_BE       0x7F000000U

#define NET_TCP_EPHEMERAL_PORT_MIN         49152U
#define NET_TCP_EPHEMERAL_PORT_MAX         65535U

#define NET_TCP_ETH_HEADER_LEN             14U
#define NET_TCP_IPV4_MIN_HEADER_LEN        20U
#define NET_TCP_HEADER_LEN                 20U
#define NET_TCP_HEADER_WORDS               5U
#define NET_TCP_ETHERTYPE_IPV4             0x0800U
#define NET_TCP_IPV4_PROTO_TCP             6U
#define NET_TCP_IPV4_TTL_DEFAULT           64U
#define NET_TCP_IPV4_FLAGS_DF              0x4000U
#define NET_TCP_ETH_FRAME_MAX_BYTES        1518U
#define NET_TCP_WINDOW_DEFAULT             65535U
#define NET_TCP_INITIAL_SEQUENCE_SEED      0x10203040U
#define NET_TCP_INITIAL_SEQUENCE_STRIDE    0x00010000U
#define NET_TCP_CONNECT_TIMEOUT_MS         3000U

#define NET_TCP_FLAG_FIN                   0x01U
#define NET_TCP_FLAG_SYN                   0x02U
#define NET_TCP_FLAG_RST                   0x04U
#define NET_TCP_FLAG_PSH                   0x08U
#define NET_TCP_FLAG_ACK                   0x10U

typedef enum net_tcp_state
{
    NET_TCP_STATE_CLOSED = 0,
    NET_TCP_STATE_LISTEN,
    NET_TCP_STATE_SYN_SENT,
    NET_TCP_STATE_SYN_RECV,
    NET_TCP_STATE_ESTABLISHED,
    NET_TCP_STATE_CLOSE_WAIT
} net_tcp_state_t;

typedef struct net_tcp_entry
{
    bool used;
    uint32_t owner_pid;
    bool waitq_ready;
    bool non_blocking;
    bool bound;
    bool listening;
    bool connected;
    bool peer_closed;
    uint16_t local_port;
    uint32_t local_addr_be;
    uint16_t peer_port;
    uint32_t peer_addr_be;
    uint16_t listen_backlog;
    uint32_t parent_listener_id;
    net_tcp_state_t state;
    uint32_t snd_iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint16_t rx_head;
    uint16_t rx_tail;
    uint32_t rx_count;
    uint16_t accept_head;
    uint16_t accept_tail;
    uint32_t accept_count;
    task_wait_queue_t rx_waitq;
    task_wait_queue_t state_waitq;
    task_wait_queue_t accept_waitq;
    uint32_t accept_queue[NET_TCP_ACCEPT_QUEUE_LEN];
    uint8_t rx_buf[NET_TCP_RX_BUFFER_BYTES];
} net_tcp_entry_t;

typedef struct net_tcp_runtime_state
{
    bool lock_ready;
    bool initialized;
    spinlock_t lock;
    uint16_t next_ephemeral_port;
    uint16_t next_ipv4_identification;
    uint32_t next_initial_sequence;
    net_tcp_entry_t entries[NET_TCP_MAX_SOCKETS];
} net_tcp_runtime_state_t;

void NET_tcp_init(void);
bool NET_tcp_is_ready(void);
bool NET_tcp_create(uint32_t owner_pid, uint32_t* out_socket_id);
bool NET_tcp_close(uint32_t owner_pid, uint32_t socket_id);
bool NET_tcp_set_non_blocking(uint32_t owner_pid, uint32_t socket_id, bool non_blocking, bool* out_non_blocking);
bool NET_tcp_bind(uint32_t owner_pid, uint32_t socket_id, uint32_t local_addr_be, uint16_t local_port);
bool NET_tcp_listen(uint32_t owner_pid, uint32_t socket_id, uint32_t backlog);
bool NET_tcp_accept(uint32_t owner_pid,
                    uint32_t socket_id,
                    uint32_t* out_child_socket_id,
                    bool force_non_blocking,
                    bool* out_would_block);
bool NET_tcp_connect(uint32_t owner_pid,
                     uint32_t socket_id,
                     uint32_t peer_addr_be,
                     uint16_t peer_port,
                     bool force_non_blocking,
                     bool* out_in_progress);
bool NET_tcp_disconnect(uint32_t owner_pid, uint32_t socket_id);
bool NET_tcp_getsockname(uint32_t owner_pid,
                         uint32_t socket_id,
                         uint32_t* out_local_addr_be,
                         uint16_t* out_local_port);
bool NET_tcp_getpeername(uint32_t owner_pid,
                         uint32_t socket_id,
                         bool* out_connected,
                         uint32_t* out_peer_addr_be,
                         uint16_t* out_peer_port);
bool NET_tcp_send(uint32_t owner_pid,
                  uint32_t socket_id,
                  const uint8_t* payload,
                  size_t payload_len,
                  size_t* out_sent,
                  bool force_non_blocking,
                  bool* out_would_block);
bool NET_tcp_recv(uint32_t owner_pid,
                  uint32_t socket_id,
                  uint8_t* out_payload,
                  size_t out_cap,
                  size_t* out_len,
                  bool force_non_blocking,
                  bool* out_would_block);
bool NET_tcp_pending_bytes(uint32_t owner_pid, uint32_t socket_id, size_t* out_bytes);
void NET_tcp_on_ethernet_frame(const uint8_t* frame, size_t frame_len);

#endif
