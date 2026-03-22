#include <Network/TCP.h>
#include <Network/TCP_private.h>

#include <Device/E1000.h>
#include <Network/ARP.h>

#include <string.h>

static net_tcp_runtime_state_t NET_tcp_state;

static inline uint16_t NET_tcp_read_be16(const uint8_t* bytes)
{
    if (!bytes)
        return 0U;

    return (uint16_t) (((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1]);
}

static inline uint32_t NET_tcp_read_be32(const uint8_t* bytes)
{
    if (!bytes)
        return 0U;

    return ((uint32_t) bytes[0] << 24) |
           ((uint32_t) bytes[1] << 16) |
           ((uint32_t) bytes[2] << 8) |
           (uint32_t) bytes[3];
}

static inline void NET_tcp_write_be16(uint8_t* bytes, uint16_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static inline void NET_tcp_write_be32(uint8_t* bytes, uint32_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static inline bool NET_tcp_is_loopback_ipv4(uint32_t addr_be)
{
    return (addr_be & NET_TCP_IPV4_LOOPBACK_MASK_BE) == NET_TCP_IPV4_LOOPBACK_NET_BE;
}

static bool NET_tcp_ensure_initialized(void)
{
    if (!NET_tcp_state.lock_ready)
    {
        spinlock_init(&NET_tcp_state.lock);
        NET_tcp_state.lock_ready = true;
    }

    if (!NET_tcp_state.initialized)
        NET_tcp_init();

    return NET_tcp_state.initialized;
}

static uint16_t NET_tcp_checksum_finalize(uint32_t sum)
{
    while ((sum >> 16) != 0U)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t) (~sum & 0xFFFFU);
}

static uint16_t NET_tcp_checksum_bytes(const uint8_t* data, size_t len)
{
    if (!data || len == 0U)
        return 0U;

    uint32_t sum = 0U;
    size_t index = 0U;
    while ((index + 1U) < len)
    {
        uint16_t word = (uint16_t) (((uint16_t) data[index] << 8) | (uint16_t) data[index + 1U]);
        sum += (uint32_t) word;
        index += 2U;
    }

    if (index < len)
        sum += (uint32_t) ((uint16_t) data[index] << 8);

    return NET_tcp_checksum_finalize(sum);
}

static uint16_t NET_tcp_segment_checksum(uint32_t src_addr_be, uint32_t dst_addr_be, const uint8_t* segment, size_t segment_len)
{
    if (!segment || segment_len == 0U)
        return 0U;

    uint32_t sum = 0U;
    sum += (src_addr_be >> 16) & 0xFFFFU;
    sum += src_addr_be & 0xFFFFU;
    sum += (dst_addr_be >> 16) & 0xFFFFU;
    sum += dst_addr_be & 0xFFFFU;
    sum += NET_TCP_IPV4_PROTO_TCP;
    sum += (uint32_t) segment_len;

    size_t index = 0U;
    while ((index + 1U) < segment_len)
    {
        uint16_t word = (uint16_t) (((uint16_t) segment[index] << 8) |
                                    (uint16_t) segment[index + 1U]);
        sum += (uint32_t) word;
        index += 2U;
    }

    if (index < segment_len)
        sum += (uint32_t) ((uint16_t) segment[index] << 8);

    uint16_t checksum = NET_tcp_checksum_finalize(sum);
    return (checksum == 0U) ? 0xFFFFU : checksum;
}

static bool NET_tcp_port_conflict_locked(uint32_t exclude_socket_id, uint32_t local_addr_be, uint16_t local_port)
{
    if (local_port == 0U)
        return false;

    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; i++)
    {
        if ((i + 1U) == exclude_socket_id)
            continue;

        const net_tcp_entry_t* entry = &NET_tcp_state.entries[i];
        if (!entry->used || !entry->bound)
            continue;
        if (entry->local_port != local_port)
            continue;

        if (entry->local_addr_be != NET_TCP_IPV4_ANY &&
            local_addr_be != NET_TCP_IPV4_ANY &&
            entry->local_addr_be != local_addr_be)
        {
            continue;
        }

        return true;
    }

    return false;
}

static uint16_t NET_tcp_alloc_ephemeral_port_locked(uint32_t local_addr_be)
{
    uint16_t candidate = NET_tcp_state.next_ephemeral_port;
    if (candidate < NET_TCP_EPHEMERAL_PORT_MIN || candidate > NET_TCP_EPHEMERAL_PORT_MAX)
        candidate = NET_TCP_EPHEMERAL_PORT_MIN;

    uint32_t span = (uint32_t) (NET_TCP_EPHEMERAL_PORT_MAX - NET_TCP_EPHEMERAL_PORT_MIN + 1U);
    for (uint32_t i = 0; i < span; i++)
    {
        uint16_t port = (uint16_t) (candidate + i);
        if (port > NET_TCP_EPHEMERAL_PORT_MAX)
            port = (uint16_t) (NET_TCP_EPHEMERAL_PORT_MIN + (port - NET_TCP_EPHEMERAL_PORT_MAX - 1U));

        if (NET_tcp_port_conflict_locked(0U, local_addr_be, port))
            continue;

        uint16_t next = (uint16_t) (port + 1U);
        if (next > NET_TCP_EPHEMERAL_PORT_MAX)
            next = NET_TCP_EPHEMERAL_PORT_MIN;
        NET_tcp_state.next_ephemeral_port = next;
        return port;
    }

    return 0U;
}

static bool NET_tcp_auto_bind_locked(net_tcp_entry_t* entry)
{
    if (!entry)
        return false;
    if (entry->bound)
        return true;

    uint16_t auto_port = NET_tcp_alloc_ephemeral_port_locked(NET_TCP_IPV4_ANY);
    if (auto_port == 0U)
        return false;

    entry->bound = true;
    entry->local_addr_be = NET_TCP_IPV4_ANY;
    entry->local_port = auto_port;
    return true;
}

static bool NET_tcp_wait_predicate(void* context)
{
    net_tcp_wait_context_t* wait_ctx = (net_tcp_wait_context_t*) context;
    if (!wait_ctx || !wait_ctx->entry)
        return false;

    const net_tcp_entry_t* entry = wait_ctx->entry;
    switch (wait_ctx->kind)
    {
        case NET_TCP_WAIT_RX:
            return entry->used &&
                   __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE) == 0U &&
                   !entry->peer_closed;

        case NET_TCP_WAIT_ACCEPT:
            return entry->used &&
                   entry->listening &&
                   __atomic_load_n(&entry->accept_count, __ATOMIC_ACQUIRE) == 0U;

        case NET_TCP_WAIT_CONNECT:
            return entry->used && entry->state == NET_TCP_STATE_SYN_SENT;

        default:
            return false;
    }
}

static bool NET_tcp_get_entry_locked(uint32_t owner_pid, uint32_t socket_id, net_tcp_entry_t** out_entry)
{
    if (!out_entry || socket_id == 0U || socket_id > NET_TCP_MAX_SOCKETS || owner_pid == 0U)
        return false;

    net_tcp_entry_t* entry = &NET_tcp_state.entries[socket_id - 1U];
    if (!entry->used || entry->owner_pid != owner_pid)
        return false;

    *out_entry = entry;
    return true;
}

static int32_t NET_tcp_find_connection_index_locked(uint32_t src_addr_be,
                                                    uint32_t dst_addr_be,
                                                    uint16_t src_port,
                                                    uint16_t dst_port)
{
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; i++)
    {
        const net_tcp_entry_t* entry = &NET_tcp_state.entries[i];
        if (!entry->used || entry->listening || !entry->bound)
            continue;
        if (entry->local_port != dst_port)
            continue;
        if (entry->local_addr_be != NET_TCP_IPV4_ANY && entry->local_addr_be != dst_addr_be)
            continue;
        if (entry->peer_port != src_port || entry->peer_addr_be != src_addr_be)
            continue;

        return (int32_t) i;
    }

    return -1;
}

static int32_t NET_tcp_find_listener_index_locked(uint32_t dst_addr_be, uint16_t dst_port)
{
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; i++)
    {
        const net_tcp_entry_t* entry = &NET_tcp_state.entries[i];
        if (!entry->used || !entry->listening || entry->state != NET_TCP_STATE_LISTEN || !entry->bound)
            continue;
        if (entry->local_port != dst_port)
            continue;
        if (entry->local_addr_be != NET_TCP_IPV4_ANY && entry->local_addr_be != dst_addr_be)
            continue;

        return (int32_t) i;
    }

    return -1;
}

static int32_t NET_tcp_alloc_entry_index_locked(void)
{
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; i++)
    {
        if (!NET_tcp_state.entries[i].used)
            return (int32_t) i;
    }

    return -1;
}

static bool NET_tcp_emit_segment(uint32_t src_addr_be,
                                 uint32_t dst_addr_be,
                                 uint16_t src_port,
                                 uint16_t dst_port,
                                 uint32_t seq,
                                 uint32_t ack,
                                 uint8_t flags,
                                 const uint8_t* payload,
                                 size_t payload_len)
{
    if (src_port == 0U || dst_port == 0U || payload_len > NET_TCP_MAX_PAYLOAD)
        return false;
    if (payload_len != 0U && !payload)
        return false;

    if (src_addr_be == NET_TCP_IPV4_ANY && NET_tcp_is_loopback_ipv4(dst_addr_be))
        src_addr_be = NET_TCP_IPV4_LOOPBACK_BE;

    if (NET_tcp_is_loopback_ipv4(dst_addr_be))
    {
        NET_tcp_process_segment(src_addr_be, dst_addr_be, src_port, dst_port, seq, ack, flags, payload, payload_len);
        return true;
    }

    if (src_addr_be == NET_TCP_IPV4_ANY || !E1000_is_available())
        return false;

    uint8_t src_mac[6];
    if (!E1000_get_mac(src_mac))
        return false;

    uint8_t dst_mac[6];
    if (!ARP_lookup_ipv4(dst_addr_be, dst_mac, NULL))
        return false;

    size_t tcp_len = NET_TCP_HEADER_LEN + payload_len;
    size_t ip_len = NET_TCP_IPV4_MIN_HEADER_LEN + tcp_len;
    size_t frame_len = NET_TCP_ETH_HEADER_LEN + ip_len;
    if (frame_len > NET_TCP_ETH_FRAME_MAX_BYTES)
        return false;

    uint8_t frame[NET_TCP_ETH_FRAME_MAX_BYTES];
    memset(frame, 0, frame_len);

    memcpy(frame + 0, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    NET_tcp_write_be16(frame + 12, NET_TCP_ETHERTYPE_IPV4);

    net_tcp_ipv4_header_t* ip = (net_tcp_ipv4_header_t*) (frame + NET_TCP_ETH_HEADER_LEN);
    ip->version_ihl = 0x45U;
    ip->dscp_ecn = 0U;
    NET_tcp_write_be16((uint8_t*) &ip->total_len_be, (uint16_t) ip_len);

    uint16_t ip_id = 0U;
    spin_lock(&NET_tcp_state.lock);
    ip_id = NET_tcp_state.next_ipv4_identification++;
    if (NET_tcp_state.next_ipv4_identification == 0U)
        NET_tcp_state.next_ipv4_identification = 1U;
    spin_unlock(&NET_tcp_state.lock);

    NET_tcp_write_be16((uint8_t*) &ip->identification_be, ip_id);
    NET_tcp_write_be16((uint8_t*) &ip->flags_frag_be, NET_TCP_IPV4_FLAGS_DF);
    ip->ttl = NET_TCP_IPV4_TTL_DEFAULT;
    ip->protocol = NET_TCP_IPV4_PROTO_TCP;
    ip->checksum_be = 0U;
    NET_tcp_write_be32((uint8_t*) &ip->src_addr_be, src_addr_be);
    NET_tcp_write_be32((uint8_t*) &ip->dst_addr_be, dst_addr_be);
    uint16_t ip_checksum = NET_tcp_checksum_bytes((const uint8_t*) ip, NET_TCP_IPV4_MIN_HEADER_LEN);
    NET_tcp_write_be16((uint8_t*) &ip->checksum_be, ip_checksum);

    net_tcp_header_t* tcp = (net_tcp_header_t*) (frame + NET_TCP_ETH_HEADER_LEN + NET_TCP_IPV4_MIN_HEADER_LEN);
    NET_tcp_write_be16((uint8_t*) &tcp->src_port_be, src_port);
    NET_tcp_write_be16((uint8_t*) &tcp->dst_port_be, dst_port);
    NET_tcp_write_be32((uint8_t*) &tcp->seq_be, seq);
    NET_tcp_write_be32((uint8_t*) &tcp->ack_be, ack);
    tcp->data_off_reserved = (uint8_t) (NET_TCP_HEADER_WORDS << 4U);
    tcp->flags = flags;
    NET_tcp_write_be16((uint8_t*) &tcp->window_be, NET_TCP_WINDOW_DEFAULT);
    NET_tcp_write_be16((uint8_t*) &tcp->checksum_be, 0U);
    NET_tcp_write_be16((uint8_t*) &tcp->urgent_ptr_be, 0U);
    if (payload_len != 0U)
        memcpy((uint8_t*) (tcp + 1), payload, payload_len);
    uint16_t tcp_checksum = NET_tcp_segment_checksum(src_addr_be, dst_addr_be, (const uint8_t*) tcp, tcp_len);
    NET_tcp_write_be16((uint8_t*) &tcp->checksum_be, tcp_checksum);

    size_t written = 0U;
    if (!E1000_raw_write(frame, frame_len, &written))
        return false;
    return written == frame_len;
}

static bool NET_tcp_queue_accept_locked(net_tcp_entry_t* listener, uint32_t child_socket_id)
{
    if (!listener || !listener->listening || listener->listen_backlog == 0U)
        return false;

    uint32_t queue_count = __atomic_load_n(&listener->accept_count, __ATOMIC_ACQUIRE);
    uint32_t queue_cap = listener->listen_backlog;
    if (queue_cap > NET_TCP_ACCEPT_QUEUE_LEN)
        queue_cap = NET_TCP_ACCEPT_QUEUE_LEN;
    if (queue_count >= queue_cap)
        return false;

    listener->accept_queue[listener->accept_tail] = child_socket_id;
    listener->accept_tail = (uint16_t) ((listener->accept_tail + 1U) % NET_TCP_ACCEPT_QUEUE_LEN);
    __atomic_store_n(&listener->accept_count, queue_count + 1U, __ATOMIC_RELEASE);
    if (listener->waitq_ready)
        task_wait_queue_wake_all(&listener->accept_waitq);
    return true;
}

static void NET_tcp_process_segment(uint32_t src_addr_be,
                                    uint32_t dst_addr_be,
                                    uint16_t src_port,
                                    uint16_t dst_port,
                                    uint32_t seq,
                                    uint32_t ack,
                                    uint8_t flags,
                                    const uint8_t* payload,
                                    size_t payload_len)
{
    if (src_port == 0U || dst_port == 0U || payload_len > NET_TCP_MAX_PAYLOAD)
        return;

    if (!NET_tcp_ensure_initialized())
        return;

    net_tcp_tx_plan_t tx = { 0 };

    spin_lock(&NET_tcp_state.lock);
    int32_t conn_index = NET_tcp_find_connection_index_locked(src_addr_be, dst_addr_be, src_port, dst_port);
    if (conn_index >= 0)
    {
        net_tcp_entry_t* entry = &NET_tcp_state.entries[(uint32_t) conn_index];

        if ((flags & NET_TCP_FLAG_RST) != 0U)
        {
            entry->peer_closed = true;
            entry->connected = false;
            entry->state = NET_TCP_STATE_CLOSED;
            if (entry->waitq_ready)
            {
                task_wait_queue_wake_all(&entry->rx_waitq);
                task_wait_queue_wake_all(&entry->state_waitq);
            }
            spin_unlock(&NET_tcp_state.lock);
            return;
        }

        if (entry->state == NET_TCP_STATE_SYN_SENT)
        {
            if ((flags & NET_TCP_FLAG_SYN) != 0U &&
                (flags & NET_TCP_FLAG_ACK) != 0U &&
                ack == entry->snd_nxt)
            {
                entry->snd_una = ack;
                entry->rcv_nxt = seq + 1U;
                entry->state = NET_TCP_STATE_ESTABLISHED;
                entry->connected = true;
                tx.valid = true;
                tx.src_addr_be = entry->local_addr_be;
                if (tx.src_addr_be == NET_TCP_IPV4_ANY)
                    tx.src_addr_be = NET_tcp_is_loopback_ipv4(entry->peer_addr_be) ? NET_TCP_IPV4_LOOPBACK_BE : NET_TCP_IPV4_ANY;
                tx.dst_addr_be = entry->peer_addr_be;
                tx.src_port = entry->local_port;
                tx.dst_port = entry->peer_port;
                tx.seq = entry->snd_nxt;
                tx.ack = entry->rcv_nxt;
                tx.flags = NET_TCP_FLAG_ACK;
                tx.payload = NULL;
                tx.payload_len = 0U;
                if (entry->waitq_ready)
                    task_wait_queue_wake_all(&entry->state_waitq);
            }

            spin_unlock(&NET_tcp_state.lock);
            if (tx.valid)
                (void) NET_tcp_emit_segment(tx.src_addr_be, tx.dst_addr_be, tx.src_port, tx.dst_port, tx.seq, tx.ack, tx.flags, tx.payload, tx.payload_len);
            return;
        }

        if (entry->state == NET_TCP_STATE_SYN_RECV)
        {
            if ((flags & NET_TCP_FLAG_ACK) != 0U && ack == entry->snd_nxt)
            {
                entry->snd_una = ack;
                entry->state = NET_TCP_STATE_ESTABLISHED;
                entry->connected = true;
                if (entry->parent_listener_id != 0U && entry->parent_listener_id <= NET_TCP_MAX_SOCKETS)
                {
                    net_tcp_entry_t* listener = &NET_tcp_state.entries[entry->parent_listener_id - 1U];
                    if (listener->used && listener->listening && listener->owner_pid == entry->owner_pid)
                    {
                        if (!NET_tcp_queue_accept_locked(listener, (uint32_t) conn_index + 1U))
                        {
                            entry->connected = false;
                            entry->used = false;
                            entry->state = NET_TCP_STATE_CLOSED;
                        }
                    }
                    else
                    {
                        entry->connected = false;
                        entry->used = false;
                        entry->state = NET_TCP_STATE_CLOSED;
                    }
                }
                if (entry->waitq_ready)
                    task_wait_queue_wake_all(&entry->state_waitq);
            }

            spin_unlock(&NET_tcp_state.lock);
            return;
        }

        if ((flags & NET_TCP_FLAG_ACK) != 0U)
        {
            if (ack > entry->snd_una && ack <= entry->snd_nxt)
                entry->snd_una = ack;
        }

        bool need_ack = false;
        if (payload_len != 0U)
        {
            if (seq == entry->rcv_nxt)
            {
                uint32_t rx_count = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
                size_t space = (rx_count < NET_TCP_RX_BUFFER_BYTES) ? (NET_TCP_RX_BUFFER_BYTES - rx_count) : 0U;
                size_t copy_len = payload_len;
                if (copy_len > space)
                    copy_len = space;

                for (size_t i = 0; i < copy_len; i++)
                {
                    entry->rx_buf[entry->rx_tail] = payload[i];
                    entry->rx_tail = (uint16_t) ((entry->rx_tail + 1U) % NET_TCP_RX_BUFFER_BYTES);
                }
                __atomic_store_n(&entry->rx_count, rx_count + (uint32_t) copy_len, __ATOMIC_RELEASE);
                entry->rcv_nxt += (uint32_t) copy_len;
                if (entry->waitq_ready && copy_len != 0U)
                    task_wait_queue_wake_all(&entry->rx_waitq);
            }
            need_ack = true;
        }

        if ((flags & NET_TCP_FLAG_FIN) != 0U)
        {
            if (seq + (uint32_t) payload_len == entry->rcv_nxt)
                entry->rcv_nxt += 1U;
            entry->peer_closed = true;
            if (entry->state == NET_TCP_STATE_ESTABLISHED)
                entry->state = NET_TCP_STATE_CLOSE_WAIT;
            if (entry->waitq_ready)
            {
                task_wait_queue_wake_all(&entry->rx_waitq);
                task_wait_queue_wake_all(&entry->state_waitq);
            }
            need_ack = true;
        }

        if (need_ack && entry->connected)
        {
            tx.valid = true;
            tx.src_addr_be = entry->local_addr_be;
            if (tx.src_addr_be == NET_TCP_IPV4_ANY)
                tx.src_addr_be = NET_tcp_is_loopback_ipv4(entry->peer_addr_be) ? NET_TCP_IPV4_LOOPBACK_BE : NET_TCP_IPV4_ANY;
            tx.dst_addr_be = entry->peer_addr_be;
            tx.src_port = entry->local_port;
            tx.dst_port = entry->peer_port;
            tx.seq = entry->snd_nxt;
            tx.ack = entry->rcv_nxt;
            tx.flags = NET_TCP_FLAG_ACK;
            tx.payload = NULL;
            tx.payload_len = 0U;
        }

        spin_unlock(&NET_tcp_state.lock);
        if (tx.valid)
            (void) NET_tcp_emit_segment(tx.src_addr_be, tx.dst_addr_be, tx.src_port, tx.dst_port, tx.seq, tx.ack, tx.flags, tx.payload, tx.payload_len);
        return;
    }

    if ((flags & NET_TCP_FLAG_SYN) != 0U && (flags & NET_TCP_FLAG_ACK) == 0U)
    {
        int32_t listener_index = NET_tcp_find_listener_index_locked(dst_addr_be, dst_port);
        if (listener_index >= 0)
        {
            net_tcp_entry_t* listener = &NET_tcp_state.entries[(uint32_t) listener_index];
            int32_t child_index = NET_tcp_alloc_entry_index_locked();
            if (child_index >= 0)
            {
                net_tcp_entry_t* child = &NET_tcp_state.entries[(uint32_t) child_index];
                memset(child, 0, sizeof(*child));
                child->used = true;
                child->owner_pid = listener->owner_pid;
                child->bound = true;
                child->non_blocking = listener->non_blocking;
                child->local_addr_be = (listener->local_addr_be == NET_TCP_IPV4_ANY) ? dst_addr_be : listener->local_addr_be;
                child->local_port = dst_port;
                child->peer_addr_be = src_addr_be;
                child->peer_port = src_port;
                child->state = NET_TCP_STATE_SYN_RECV;
                child->connected = false;
                child->peer_closed = false;
                child->listen_backlog = 0U;
                child->parent_listener_id = (uint32_t) listener_index + 1U;
                task_wait_queue_init(&child->rx_waitq);
                task_wait_queue_init(&child->state_waitq);
                task_wait_queue_init(&child->accept_waitq);
                child->waitq_ready = true;
                child->snd_iss = NET_tcp_state.next_initial_sequence;
                NET_tcp_state.next_initial_sequence += NET_TCP_INITIAL_SEQUENCE_STRIDE;
                child->snd_una = child->snd_iss;
                child->snd_nxt = child->snd_iss + 1U;
                child->rcv_nxt = seq + 1U;

                tx.valid = true;
                tx.src_addr_be = child->local_addr_be;
                tx.dst_addr_be = child->peer_addr_be;
                tx.src_port = child->local_port;
                tx.dst_port = child->peer_port;
                tx.seq = child->snd_iss;
                tx.ack = child->rcv_nxt;
                tx.flags = (uint8_t) (NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK);
                tx.payload = NULL;
                tx.payload_len = 0U;
            }
        }
    }

    spin_unlock(&NET_tcp_state.lock);
    if (tx.valid)
        (void) NET_tcp_emit_segment(tx.src_addr_be, tx.dst_addr_be, tx.src_port, tx.dst_port, tx.seq, tx.ack, tx.flags, tx.payload, tx.payload_len);
}

void NET_tcp_init(void)
{
    if (!NET_tcp_state.lock_ready)
    {
        spinlock_init(&NET_tcp_state.lock);
        NET_tcp_state.lock_ready = true;
    }

    spin_lock(&NET_tcp_state.lock);
    if (!NET_tcp_state.initialized)
    {
        memset(NET_tcp_state.entries, 0, sizeof(NET_tcp_state.entries));
        NET_tcp_state.next_ephemeral_port = NET_TCP_EPHEMERAL_PORT_MIN;
        NET_tcp_state.next_ipv4_identification = 1U;
        NET_tcp_state.next_initial_sequence = NET_TCP_INITIAL_SEQUENCE_SEED;
        NET_tcp_state.initialized = true;
    }
    spin_unlock(&NET_tcp_state.lock);
}

bool NET_tcp_is_ready(void)
{
    return NET_tcp_state.initialized;
}

bool NET_tcp_create(uint32_t owner_pid, uint32_t* out_socket_id)
{
    if (!out_socket_id || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    spin_lock(&NET_tcp_state.lock);
    int32_t index = NET_tcp_alloc_entry_index_locked();
    if (index < 0)
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    net_tcp_entry_t* entry = &NET_tcp_state.entries[(uint32_t) index];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->owner_pid = owner_pid;
    entry->local_addr_be = NET_TCP_IPV4_ANY;
    entry->state = NET_TCP_STATE_CLOSED;
    task_wait_queue_init(&entry->rx_waitq);
    task_wait_queue_init(&entry->state_waitq);
    task_wait_queue_init(&entry->accept_waitq);
    entry->waitq_ready = true;

    *out_socket_id = (uint32_t) index + 1U;
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_close(uint32_t owner_pid, uint32_t socket_id)
{
    if (owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (entry->waitq_ready)
    {
        task_wait_queue_wake_all(&entry->rx_waitq);
        task_wait_queue_wake_all(&entry->state_waitq);
        task_wait_queue_wake_all(&entry->accept_waitq);
    }

    if (entry->listening)
    {
        for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; i++)
        {
            net_tcp_entry_t* child = &NET_tcp_state.entries[i];
            if (!child->used || child->owner_pid != owner_pid)
                continue;
            if (child->parent_listener_id != socket_id)
                continue;
            if (child->waitq_ready)
            {
                task_wait_queue_wake_all(&child->rx_waitq);
                task_wait_queue_wake_all(&child->state_waitq);
            }
            memset(child, 0, sizeof(*child));
        }
    }

    memset(entry, 0, sizeof(*entry));
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_set_non_blocking(uint32_t owner_pid, uint32_t socket_id, bool non_blocking, bool* out_non_blocking)
{
    if (owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    entry->non_blocking = non_blocking;
    if (out_non_blocking)
        *out_non_blocking = entry->non_blocking;
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_bind(uint32_t owner_pid, uint32_t socket_id, uint32_t local_addr_be, uint16_t local_port)
{
    if (owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (entry->connected ||
        entry->listening ||
        entry->state == NET_TCP_STATE_LISTEN ||
        entry->state == NET_TCP_STATE_SYN_SENT ||
        entry->state == NET_TCP_STATE_SYN_RECV ||
        entry->state == NET_TCP_STATE_ESTABLISHED)
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (local_port == 0U)
    {
        local_port = NET_tcp_alloc_ephemeral_port_locked(local_addr_be);
        if (local_port == 0U)
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }
    }

    if (NET_tcp_port_conflict_locked(socket_id, local_addr_be, local_port))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    entry->bound = true;
    entry->local_addr_be = local_addr_be;
    entry->local_port = local_port;
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_listen(uint32_t owner_pid, uint32_t socket_id, uint32_t backlog)
{
    if (owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    if (backlog == 0U)
        backlog = 1U;
    if (backlog > NET_TCP_ACCEPT_QUEUE_LEN)
        backlog = NET_TCP_ACCEPT_QUEUE_LEN;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }
    if (entry->connected ||
        entry->state == NET_TCP_STATE_ESTABLISHED ||
        entry->state == NET_TCP_STATE_SYN_SENT ||
        entry->state == NET_TCP_STATE_SYN_RECV)
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (!NET_tcp_auto_bind_locked(entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    entry->listening = true;
    entry->connected = false;
    entry->peer_closed = false;
    entry->state = NET_TCP_STATE_LISTEN;
    entry->listen_backlog = (uint16_t) backlog;
    entry->accept_head = 0U;
    entry->accept_tail = 0U;
    __atomic_store_n(&entry->accept_count, 0U, __ATOMIC_RELEASE);
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_accept(uint32_t owner_pid,
                    uint32_t socket_id,
                    uint32_t* out_child_socket_id,
                    bool force_non_blocking,
                    bool* out_would_block)
{
    if (!out_child_socket_id || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    *out_child_socket_id = 0U;
    if (out_would_block)
        *out_would_block = false;

    for (;;)
    {
        net_tcp_wait_context_t wait_ctx = { 0 };
        bool should_block = false;
        task_wait_queue_t* waitq = NULL;

        spin_lock(&NET_tcp_state.lock);
        net_tcp_entry_t* entry = NULL;
        if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }
        if (!entry->listening || entry->state != NET_TCP_STATE_LISTEN)
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }

        uint32_t queue_count = __atomic_load_n(&entry->accept_count, __ATOMIC_ACQUIRE);
        if (queue_count != 0U)
        {
            uint32_t child_socket_id = entry->accept_queue[entry->accept_head];
            entry->accept_queue[entry->accept_head] = 0U;
            entry->accept_head = (uint16_t) ((entry->accept_head + 1U) % NET_TCP_ACCEPT_QUEUE_LEN);
            __atomic_store_n(&entry->accept_count, queue_count - 1U, __ATOMIC_RELEASE);
            spin_unlock(&NET_tcp_state.lock);

            if (child_socket_id == 0U || child_socket_id > NET_TCP_MAX_SOCKETS)
                continue;

            spin_lock(&NET_tcp_state.lock);
            bool valid_child = NET_tcp_state.entries[child_socket_id - 1U].used &&
                               NET_tcp_state.entries[child_socket_id - 1U].owner_pid == owner_pid;
            spin_unlock(&NET_tcp_state.lock);
            if (!valid_child)
                continue;

            *out_child_socket_id = child_socket_id;
            return true;
        }

        should_block = !force_non_blocking && !entry->non_blocking && entry->waitq_ready;
        waitq = &entry->accept_waitq;
        wait_ctx.entry = entry;
        wait_ctx.kind = NET_TCP_WAIT_ACCEPT;
        spin_unlock(&NET_tcp_state.lock);

        if (!should_block)
        {
            if (out_would_block)
                *out_would_block = true;
            return true;
        }

        task_waiter_t waiter = { 0 };
        if (!task_wait_queue_wait_event(waitq,
                                        &waiter,
                                        NET_tcp_wait_predicate,
                                        &wait_ctx,
                                        TASK_WAIT_TIMEOUT_INFINITE))
        {
            return false;
        }
    }
}

bool NET_tcp_connect(uint32_t owner_pid,
                     uint32_t socket_id,
                     uint32_t peer_addr_be,
                     uint16_t peer_port,
                     bool force_non_blocking,
                     bool* out_in_progress)
{
    if (owner_pid == 0U || peer_addr_be == NET_TCP_IPV4_ANY || peer_port == 0U || !NET_tcp_ensure_initialized())
        return false;

    if (out_in_progress)
        *out_in_progress = false;

    net_tcp_wait_context_t wait_ctx = { 0 };
    task_wait_queue_t* waitq = NULL;

    uint32_t src_addr_be = NET_TCP_IPV4_ANY;
    uint16_t src_port = 0U;
    uint32_t syn_seq = 0U;
    bool non_blocking = false;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (entry->listening || entry->state == NET_TCP_STATE_LISTEN)
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (!NET_tcp_auto_bind_locked(entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    entry->peer_addr_be = peer_addr_be;
    entry->peer_port = peer_port;
    entry->connected = false;
    entry->peer_closed = false;
    entry->parent_listener_id = 0U;
    entry->snd_iss = NET_tcp_state.next_initial_sequence;
    NET_tcp_state.next_initial_sequence += NET_TCP_INITIAL_SEQUENCE_STRIDE;
    entry->snd_una = entry->snd_iss;
    entry->snd_nxt = entry->snd_iss + 1U;
    entry->rcv_nxt = 0U;
    entry->state = NET_TCP_STATE_SYN_SENT;
    src_addr_be = entry->local_addr_be;
    src_port = entry->local_port;
    syn_seq = entry->snd_iss;
    non_blocking = entry->non_blocking;
    waitq = &entry->state_waitq;
    wait_ctx.entry = entry;
    wait_ctx.kind = NET_TCP_WAIT_CONNECT;
    spin_unlock(&NET_tcp_state.lock);

    if (!NET_tcp_emit_segment(src_addr_be,
                              peer_addr_be,
                              src_port,
                              peer_port,
                              syn_seq,
                              0U,
                              NET_TCP_FLAG_SYN,
                              NULL,
                              0U))
    {
        spin_lock(&NET_tcp_state.lock);
        if (NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
        {
            entry->state = NET_TCP_STATE_CLOSED;
            entry->connected = false;
        }
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    if (force_non_blocking || non_blocking)
    {
        if (out_in_progress)
            *out_in_progress = true;
        return true;
    }

    task_waiter_t waiter = { 0 };
    if (!task_wait_queue_wait_event(waitq,
                                    &waiter,
                                    NET_tcp_wait_predicate,
                                    &wait_ctx,
                                    task_ticks_from_ms(NET_TCP_CONNECT_TIMEOUT_MS)))
    {
        spin_lock(&NET_tcp_state.lock);
        if (NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
        {
            entry->state = NET_TCP_STATE_CLOSED;
            entry->connected = false;
        }
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    spin_lock(&NET_tcp_state.lock);
    bool ok = NET_tcp_get_entry_locked(owner_pid, socket_id, &entry) &&
              entry->state == NET_TCP_STATE_ESTABLISHED &&
              entry->connected;
    spin_unlock(&NET_tcp_state.lock);
    return ok;
}

bool NET_tcp_disconnect(uint32_t owner_pid, uint32_t socket_id)
{
    if (owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    entry->connected = false;
    entry->peer_closed = true;
    entry->peer_addr_be = NET_TCP_IPV4_ANY;
    entry->peer_port = 0U;
    entry->state = NET_TCP_STATE_CLOSED;
    if (entry->waitq_ready)
    {
        task_wait_queue_wake_all(&entry->rx_waitq);
        task_wait_queue_wake_all(&entry->state_waitq);
    }
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_getsockname(uint32_t owner_pid,
                         uint32_t socket_id,
                         uint32_t* out_local_addr_be,
                         uint16_t* out_local_port)
{
    if (!out_local_addr_be || !out_local_port || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    *out_local_addr_be = NET_TCP_IPV4_ANY;
    *out_local_port = 0U;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    *out_local_addr_be = entry->local_addr_be;
    *out_local_port = entry->local_port;
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_getpeername(uint32_t owner_pid,
                         uint32_t socket_id,
                         bool* out_connected,
                         uint32_t* out_peer_addr_be,
                         uint16_t* out_peer_port)
{
    if (!out_connected || !out_peer_addr_be || !out_peer_port || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    *out_connected = false;
    *out_peer_addr_be = NET_TCP_IPV4_ANY;
    *out_peer_port = 0U;

    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    *out_connected = entry->connected;
    *out_peer_addr_be = entry->peer_addr_be;
    *out_peer_port = entry->peer_port;
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

bool NET_tcp_send(uint32_t owner_pid,
                  uint32_t socket_id,
                  const uint8_t* payload,
                  size_t payload_len,
                  size_t* out_sent,
                  bool force_non_blocking,
                  bool* out_would_block)
{
    if (!out_sent || owner_pid == 0U || payload_len == 0U || !payload || !NET_tcp_ensure_initialized())
        return false;

    *out_sent = 0U;
    if (out_would_block)
        *out_would_block = false;

    size_t sent = 0U;
    while (sent < payload_len)
    {
        size_t chunk_len = payload_len - sent;
        if (chunk_len > NET_TCP_MAX_PAYLOAD)
            chunk_len = NET_TCP_MAX_PAYLOAD;

        uint32_t src_addr_be = NET_TCP_IPV4_ANY;
        uint16_t src_port = 0U;
        uint32_t dst_addr_be = NET_TCP_IPV4_ANY;
        uint16_t dst_port = 0U;
        uint32_t seq = 0U;
        uint32_t ack = 0U;
        bool would_block = false;

        spin_lock(&NET_tcp_state.lock);
        net_tcp_entry_t* entry = NULL;
        if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }

        if (!entry->connected || entry->peer_addr_be == NET_TCP_IPV4_ANY || entry->peer_port == 0U)
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }

        if (entry->state != NET_TCP_STATE_ESTABLISHED && entry->state != NET_TCP_STATE_CLOSE_WAIT)
        {
            would_block = force_non_blocking || entry->non_blocking;
            spin_unlock(&NET_tcp_state.lock);
            if (out_would_block)
                *out_would_block = would_block;
            return would_block;
        }

        src_addr_be = entry->local_addr_be;
        src_port = entry->local_port;
        dst_addr_be = entry->peer_addr_be;
        dst_port = entry->peer_port;
        seq = entry->snd_nxt;
        ack = entry->rcv_nxt;
        entry->snd_nxt += (uint32_t) chunk_len;
        spin_unlock(&NET_tcp_state.lock);

        if (!NET_tcp_emit_segment(src_addr_be,
                                  dst_addr_be,
                                  src_port,
                                  dst_port,
                                  seq,
                                  ack,
                                  (uint8_t) (NET_TCP_FLAG_ACK | NET_TCP_FLAG_PSH),
                                  payload + sent,
                                  chunk_len))
        {
            if (sent == 0U)
                return false;
            break;
        }

        sent += chunk_len;
    }

    *out_sent = sent;
    return true;
}

bool NET_tcp_recv(uint32_t owner_pid,
                  uint32_t socket_id,
                  uint8_t* out_payload,
                  size_t out_cap,
                  size_t* out_len,
                  bool force_non_blocking,
                  bool* out_would_block)
{
    if (!out_payload || out_cap == 0U || !out_len || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    *out_len = 0U;
    if (out_would_block)
        *out_would_block = false;

    for (;;)
    {
        net_tcp_wait_context_t wait_ctx = { 0 };
        bool should_block = false;
        task_wait_queue_t* waitq = NULL;

        spin_lock(&NET_tcp_state.lock);
        net_tcp_entry_t* entry = NULL;
        if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
        {
            spin_unlock(&NET_tcp_state.lock);
            return false;
        }

        uint32_t rx_count = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
        if (rx_count != 0U)
        {
            size_t copy_len = rx_count;
            if (copy_len > out_cap)
                copy_len = out_cap;

            for (size_t i = 0; i < copy_len; i++)
            {
                out_payload[i] = entry->rx_buf[entry->rx_head];
                entry->rx_head = (uint16_t) ((entry->rx_head + 1U) % NET_TCP_RX_BUFFER_BYTES);
            }
            __atomic_store_n(&entry->rx_count, rx_count - (uint32_t) copy_len, __ATOMIC_RELEASE);
            spin_unlock(&NET_tcp_state.lock);
            *out_len = copy_len;
            return true;
        }

        if (entry->peer_closed)
        {
            spin_unlock(&NET_tcp_state.lock);
            *out_len = 0U;
            return true;
        }

        should_block = !force_non_blocking && !entry->non_blocking && entry->waitq_ready;
        waitq = &entry->rx_waitq;
        wait_ctx.entry = entry;
        wait_ctx.kind = NET_TCP_WAIT_RX;
        spin_unlock(&NET_tcp_state.lock);

        if (!should_block)
        {
            if (out_would_block)
                *out_would_block = true;
            return true;
        }

        task_waiter_t waiter = { 0 };
        if (!task_wait_queue_wait_event(waitq,
                                        &waiter,
                                        NET_tcp_wait_predicate,
                                        &wait_ctx,
                                        TASK_WAIT_TIMEOUT_INFINITE))
        {
            return false;
        }
    }
}

bool NET_tcp_pending_bytes(uint32_t owner_pid, uint32_t socket_id, size_t* out_bytes)
{
    if (!out_bytes || owner_pid == 0U || !NET_tcp_ensure_initialized())
        return false;

    *out_bytes = 0U;
    spin_lock(&NET_tcp_state.lock);
    net_tcp_entry_t* entry = NULL;
    if (!NET_tcp_get_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_tcp_state.lock);
        return false;
    }

    *out_bytes = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
    spin_unlock(&NET_tcp_state.lock);
    return true;
}

void NET_tcp_on_ethernet_frame(const uint8_t* frame, size_t frame_len)
{
    if (!frame || frame_len < (NET_TCP_ETH_HEADER_LEN + NET_TCP_IPV4_MIN_HEADER_LEN + NET_TCP_HEADER_LEN))
        return;
    if (!NET_tcp_ensure_initialized())
        return;

    uint16_t ethertype = NET_tcp_read_be16(frame + 12U);
    if (ethertype != NET_TCP_ETHERTYPE_IPV4)
        return;

    const net_tcp_ipv4_header_t* ip = (const net_tcp_ipv4_header_t*) (frame + NET_TCP_ETH_HEADER_LEN);
    uint8_t ihl_bytes = (uint8_t) ((ip->version_ihl & 0x0FU) * 4U);
    if ((ip->version_ihl >> 4U) != 4U || ihl_bytes < NET_TCP_IPV4_MIN_HEADER_LEN)
        return;
    if (ip->protocol != NET_TCP_IPV4_PROTO_TCP)
        return;

    size_t ip_total_len = NET_tcp_read_be16((const uint8_t*) &ip->total_len_be);
    if (ip_total_len < (size_t) ihl_bytes + NET_TCP_HEADER_LEN)
        return;
    if (frame_len < NET_TCP_ETH_HEADER_LEN + ip_total_len)
        return;

    uint16_t flags_frag = NET_tcp_read_be16((const uint8_t*) &ip->flags_frag_be);
    if ((flags_frag & 0x3FFFU) != 0U)
        return;

    const uint8_t* tcp_ptr = frame + NET_TCP_ETH_HEADER_LEN + ihl_bytes;
    const net_tcp_header_t* tcp = (const net_tcp_header_t*) tcp_ptr;
    uint8_t data_off_bytes = (uint8_t) ((tcp->data_off_reserved >> 4U) * 4U);
    if (data_off_bytes < NET_TCP_HEADER_LEN)
        return;
    if ((size_t) data_off_bytes > (ip_total_len - ihl_bytes))
        return;

    uint16_t src_port = NET_tcp_read_be16((const uint8_t*) &tcp->src_port_be);
    uint16_t dst_port = NET_tcp_read_be16((const uint8_t*) &tcp->dst_port_be);
    if (src_port == 0U || dst_port == 0U)
        return;

    uint32_t seq = NET_tcp_read_be32((const uint8_t*) &tcp->seq_be);
    uint32_t ack = NET_tcp_read_be32((const uint8_t*) &tcp->ack_be);
    uint8_t flags = tcp->flags;

    const uint8_t* payload = tcp_ptr + data_off_bytes;
    size_t payload_len = (ip_total_len - ihl_bytes) - data_off_bytes;
    if (payload_len > NET_TCP_MAX_PAYLOAD)
        return;

    uint32_t src_addr_be = NET_tcp_read_be32((const uint8_t*) &ip->src_addr_be);
    uint32_t dst_addr_be = NET_tcp_read_be32((const uint8_t*) &ip->dst_addr_be);

    NET_tcp_process_segment(src_addr_be, dst_addr_be, src_port, dst_port, seq, ack, flags, payload, payload_len);
}
