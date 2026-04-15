#ifndef _UNIX_NET_H
#define _UNIX_NET_H

#include <Debug/Spinlock.h>
#include <Task/Task.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_UNIX_AF_UNIX                1U
#define NET_UNIX_SOCK_STREAM            1U
#define NET_UNIX_SOCK_DGRAM             2U
#define NET_UNIX_MSG_DONTWAIT           0x40U

#define NET_UNIX_MAX_SOCKETS            64U
#define NET_UNIX_PATH_MAX               108U
#define NET_UNIX_RX_QUEUE_LEN           32U
#define NET_UNIX_RX_MAX_PAYLOAD         4096U
#define NET_UNIX_STREAM_BUF_BYTES       8192U
#define NET_UNIX_ACCEPT_QUEUE_LEN       16U

typedef struct net_unix_datagram
{
    uint16_t len;
    uint32_t src_socket_id;
    uint8_t payload[NET_UNIX_RX_MAX_PAYLOAD];
} net_unix_datagram_t;

typedef struct net_unix_entry
{
    bool used;
    uint32_t owner_pid;
    uint32_t socket_type;
    bool bound;
    bool listening;
    bool connected;
    bool peer_closed;
    bool non_blocking;
    bool waitq_ready;
    uint32_t peer_id;
    char path[NET_UNIX_PATH_MAX];

    /* DGRAM RX queue */
    uint16_t dgram_rx_head;
    uint16_t dgram_rx_tail;
    uint32_t dgram_rx_count;
    net_unix_datagram_t dgram_rx_queue[NET_UNIX_RX_QUEUE_LEN];

    /* STREAM byte ring buffer */
    uint16_t stream_rx_head;
    uint16_t stream_rx_tail;
    uint32_t stream_rx_count;
    uint8_t stream_rx_buf[NET_UNIX_STREAM_BUF_BYTES];

    /* Accept queue (STREAM listener) */
    uint16_t accept_head;
    uint16_t accept_tail;
    uint32_t accept_count;
    uint16_t listen_backlog;
    uint32_t accept_queue[NET_UNIX_ACCEPT_QUEUE_LEN];

    task_wait_queue_t rx_waitq;
    task_wait_queue_t accept_waitq;
    task_wait_queue_t state_waitq;
} net_unix_entry_t;

typedef struct net_unix_state
{
    bool lock_ready;
    bool initialized;
    spinlock_t lock;
    net_unix_entry_t entries[NET_UNIX_MAX_SOCKETS];
} net_unix_state_t;

void NET_unix_init(void);
bool NET_unix_is_ready(void);
bool NET_unix_create(uint32_t owner_pid, uint32_t socket_type, uint32_t* out_socket_id);
bool NET_unix_close(uint32_t owner_pid, uint32_t socket_id);
bool NET_unix_bind(uint32_t owner_pid, uint32_t socket_id, const char* path, size_t path_len);
bool NET_unix_connect(uint32_t owner_pid, uint32_t socket_id, const char* path, size_t path_len,
                      bool force_non_blocking, bool* out_would_block);
bool NET_unix_listen(uint32_t owner_pid, uint32_t socket_id, uint32_t backlog);
bool NET_unix_accept(uint32_t owner_pid, uint32_t socket_id, uint32_t* out_child_socket_id,
                     bool force_non_blocking, bool* out_would_block);
bool NET_unix_sendto(uint32_t owner_pid, uint32_t socket_id,
                     const uint8_t* payload, size_t payload_len,
                     const char* dest_path, size_t dest_path_len,
                     size_t* out_sent);
bool NET_unix_recvfrom(uint32_t owner_pid, uint32_t socket_id,
                       uint8_t* out_payload, size_t out_cap, size_t* out_len,
                       bool force_non_blocking, bool* out_would_block);
bool NET_unix_set_non_blocking(uint32_t owner_pid, uint32_t socket_id,
                               bool non_blocking, bool* out_non_blocking);

#endif
