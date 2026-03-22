#include <Network/Socket.h>
#include <Network/Socket_private.h>

#include <Device/E1000.h>
#include <Network/ARP.h>

#include <string.h>

static net_socket_runtime_state_t NET_socket_state;

static inline uint16_t NET_socket_read_be16(const uint8_t* bytes)
{
    if (!bytes)
        return 0;

    return (uint16_t) (((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1]);
}

static inline uint32_t NET_socket_read_be32(const uint8_t* bytes)
{
    if (!bytes)
        return 0;

    return ((uint32_t) bytes[0] << 24) |
           ((uint32_t) bytes[1] << 16) |
           ((uint32_t) bytes[2] << 8) |
           (uint32_t) bytes[3];
}

static inline void NET_socket_write_be16(uint8_t* bytes, uint16_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static inline void NET_socket_write_be32(uint8_t* bytes, uint32_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static inline bool NET_socket_is_loopback_ipv4(uint32_t addr_be)
{
    return (addr_be & NET_SOCKET_IPV4_LOOPBACK_MASK_BE) == NET_SOCKET_IPV4_LOOPBACK_NET_BE;
}

static bool NET_socket_ensure_initialized(void)
{
    if (!NET_socket_state.lock_ready)
    {
        spinlock_init(&NET_socket_state.lock);
        NET_socket_state.lock_ready = true;
    }

    if (!NET_socket_state.initialized)
        NET_socket_init();

    return NET_socket_state.initialized;
}

static uint16_t NET_socket_checksum_finalize(uint32_t sum)
{
    while ((sum >> 16) != 0U)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t) (~sum & 0xFFFFU);
}

static uint16_t NET_socket_checksum_bytes(const uint8_t* data, size_t len)
{
    if (!data || len == 0U)
        return 0;

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

    return NET_socket_checksum_finalize(sum);
}

static uint16_t NET_socket_udp_checksum(uint32_t src_addr_be,
                                        uint32_t dst_addr_be,
                                        const uint8_t* udp_segment,
                                        size_t udp_len)
{
    if (!udp_segment || udp_len == 0U)
        return 0;

    uint32_t sum = 0U;
    sum += (src_addr_be >> 16) & 0xFFFFU;
    sum += src_addr_be & 0xFFFFU;
    sum += (dst_addr_be >> 16) & 0xFFFFU;
    sum += dst_addr_be & 0xFFFFU;
    sum += NET_SOCKET_IPV4_PROTO_UDP;
    sum += (uint32_t) udp_len;

    size_t index = 0U;
    while ((index + 1U) < udp_len)
    {
        uint16_t word = (uint16_t) (((uint16_t) udp_segment[index] << 8) |
                                    (uint16_t) udp_segment[index + 1U]);
        sum += (uint32_t) word;
        index += 2U;
    }
    if (index < udp_len)
        sum += (uint32_t) ((uint16_t) udp_segment[index] << 8);

    uint16_t checksum = NET_socket_checksum_finalize(sum);
    return (checksum == 0U) ? 0xFFFFU : checksum;
}

static bool NET_socket_port_conflict_locked(uint32_t exclude_socket_id, uint32_t local_addr_be, uint16_t local_port)
{
    if (local_port == 0U)
        return false;

    for (uint32_t i = 0; i < NET_SOCKET_MAX_UDP_SOCKETS; i++)
    {
        if ((i + 1U) == exclude_socket_id)
            continue;

        const net_socket_udp_entry_t* entry = &NET_socket_state.udp_entries[i];
        if (!entry->used || !entry->bound)
            continue;
        if (entry->local_port != local_port)
            continue;

        if (entry->local_addr_be != NET_SOCKET_IPV4_ANY &&
            local_addr_be != NET_SOCKET_IPV4_ANY &&
            entry->local_addr_be != local_addr_be)
        {
            continue;
        }

        return true;
    }

    return false;
}

static uint16_t NET_socket_alloc_ephemeral_port_locked(uint32_t local_addr_be)
{
    uint16_t candidate = NET_socket_state.next_ephemeral_port;
    if (candidate < NET_SOCKET_EPHEMERAL_PORT_MIN || candidate > NET_SOCKET_EPHEMERAL_PORT_MAX)
        candidate = NET_SOCKET_EPHEMERAL_PORT_MIN;

    uint32_t span = (uint32_t) (NET_SOCKET_EPHEMERAL_PORT_MAX - NET_SOCKET_EPHEMERAL_PORT_MIN + 1U);
    for (uint32_t i = 0; i < span; i++)
    {
        uint16_t port = (uint16_t) (candidate + i);
        if (port > NET_SOCKET_EPHEMERAL_PORT_MAX)
            port = (uint16_t) (NET_SOCKET_EPHEMERAL_PORT_MIN + (port - NET_SOCKET_EPHEMERAL_PORT_MAX - 1U));

        if (NET_socket_port_conflict_locked(0U, local_addr_be, port))
            continue;

        uint16_t next = (uint16_t) (port + 1U);
        if (next > NET_SOCKET_EPHEMERAL_PORT_MAX)
            next = NET_SOCKET_EPHEMERAL_PORT_MIN;
        NET_socket_state.next_ephemeral_port = next;
        return port;
    }

    return 0U;
}

static bool NET_socket_auto_bind_locked(net_socket_udp_entry_t* entry)
{
    if (!entry)
        return false;
    if (entry->bound)
        return true;

    uint16_t auto_port = NET_socket_alloc_ephemeral_port_locked(NET_SOCKET_IPV4_ANY);
    if (auto_port == 0U)
        return false;

    entry->bound = true;
    entry->local_addr_be = NET_SOCKET_IPV4_ANY;
    entry->local_port = auto_port;
    return true;
}

static uint32_t NET_socket_enqueue_udp_locked(uint32_t dst_addr_be,
                                              uint16_t dst_port,
                                              uint32_t src_addr_be,
                                              uint16_t src_port,
                                              const uint8_t* payload,
                                              size_t payload_len)
{
    if (!payload || payload_len > NET_SOCKET_UDP_MAX_PAYLOAD || dst_port == 0U)
        return 0U;

    uint32_t delivered = 0U;
    for (uint32_t i = 0; i < NET_SOCKET_MAX_UDP_SOCKETS; i++)
    {
        net_socket_udp_entry_t* entry = &NET_socket_state.udp_entries[i];
        if (!entry->used || !entry->bound)
            continue;
        if (entry->local_port != dst_port)
            continue;
        if (entry->local_addr_be != NET_SOCKET_IPV4_ANY && entry->local_addr_be != dst_addr_be)
            continue;
        if (entry->connected)
        {
            if (entry->peer_port != src_port)
                continue;
            if (entry->peer_addr_be != src_addr_be)
                continue;
        }

        uint32_t queue_count = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
        if (queue_count >= NET_SOCKET_UDP_RX_QUEUE_LEN)
            continue;

        net_socket_udp_datagram_t* slot = &entry->rx_queue[entry->rx_tail];
        slot->len = (uint16_t) payload_len;
        memcpy(slot->payload, payload, payload_len);
        slot->src.ipv4_addr_be = src_addr_be;
        slot->src.port = src_port;
        slot->dst.ipv4_addr_be = dst_addr_be;
        slot->dst.port = dst_port;

        entry->rx_tail = (uint16_t) ((entry->rx_tail + 1U) % NET_SOCKET_UDP_RX_QUEUE_LEN);
        __atomic_store_n(&entry->rx_count, queue_count + 1U, __ATOMIC_RELEASE);
        delivered++;
        if (entry->waitq_ready)
            task_wait_queue_wake_all(&entry->rx_waitq);
    }

    return delivered;
}

static bool NET_socket_wait_rx_empty_predicate(void* context)
{
    net_socket_wait_context_t* wait_ctx = (net_socket_wait_context_t*) context;
    if (!wait_ctx || !wait_ctx->entry)
        return false;

    return __atomic_load_n(&wait_ctx->entry->rx_count, __ATOMIC_ACQUIRE) == 0U;
}

static bool NET_socket_get_udp_entry_locked(uint32_t owner_pid, uint32_t socket_id, net_socket_udp_entry_t** out_entry)
{
    if (!out_entry || socket_id == 0U || socket_id > NET_SOCKET_MAX_UDP_SOCKETS || owner_pid == 0U)
        return false;

    net_socket_udp_entry_t* entry = &NET_socket_state.udp_entries[socket_id - 1U];
    if (!entry->used || entry->owner_pid != owner_pid)
        return false;

    *out_entry = entry;
    return true;
}

void NET_socket_init(void)
{
    if (!NET_socket_state.lock_ready)
    {
        spinlock_init(&NET_socket_state.lock);
        NET_socket_state.lock_ready = true;
    }

    spin_lock(&NET_socket_state.lock);
    if (!NET_socket_state.initialized)
    {
        memset(NET_socket_state.udp_entries, 0, sizeof(NET_socket_state.udp_entries));
        NET_socket_state.next_ephemeral_port = NET_SOCKET_EPHEMERAL_PORT_MIN;
        NET_socket_state.next_ipv4_identification = 1U;
        NET_socket_state.initialized = true;
    }
    spin_unlock(&NET_socket_state.lock);
}

bool NET_socket_is_ready(void)
{
    return NET_socket_state.initialized;
}

bool NET_socket_create_udp(uint32_t owner_pid, uint32_t* out_socket_id)
{
    if (!out_socket_id || owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    for (uint32_t i = 0; i < NET_SOCKET_MAX_UDP_SOCKETS; i++)
    {
        net_socket_udp_entry_t* entry = &NET_socket_state.udp_entries[i];
        if (entry->used)
            continue;

        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->owner_pid = owner_pid;
        entry->local_addr_be = NET_SOCKET_IPV4_ANY;
        if (!entry->waitq_ready)
        {
            task_wait_queue_init(&entry->rx_waitq);
            entry->waitq_ready = true;
        }
        *out_socket_id = i + 1U;
        spin_unlock(&NET_socket_state.lock);
        return true;
    }
    spin_unlock(&NET_socket_state.lock);
    return false;
}

bool NET_socket_close_udp(uint32_t owner_pid, uint32_t socket_id)
{
    if (owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    bool waitq_ready = entry->waitq_ready;
    memset(entry, 0, sizeof(*entry));
    if (waitq_ready)
    {
        task_wait_queue_init(&entry->rx_waitq);
        entry->waitq_ready = true;
        task_wait_queue_wake_all(&entry->rx_waitq);
    }
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_set_non_blocking(uint32_t owner_pid, uint32_t socket_id, bool non_blocking, bool* out_non_blocking)
{
    if (owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    entry->non_blocking = non_blocking;
    if (out_non_blocking)
        *out_non_blocking = entry->non_blocking;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_bind_udp(uint32_t owner_pid, uint32_t socket_id, uint32_t local_addr_be, uint16_t local_port)
{
    if (owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    if (local_port == 0U)
    {
        local_port = NET_socket_alloc_ephemeral_port_locked(local_addr_be);
        if (local_port == 0U)
        {
            spin_unlock(&NET_socket_state.lock);
            return false;
        }
    }

    if (NET_socket_port_conflict_locked(socket_id, local_addr_be, local_port))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    entry->local_addr_be = local_addr_be;
    entry->local_port = local_port;
    entry->bound = true;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_connect_udp(uint32_t owner_pid, uint32_t socket_id, uint32_t peer_addr_be, uint16_t peer_port)
{
    if (owner_pid == 0U || peer_port == 0U || peer_addr_be == NET_SOCKET_IPV4_ANY || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    if (!NET_socket_auto_bind_locked(entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    entry->connected = true;
    entry->peer_addr_be = peer_addr_be;
    entry->peer_port = peer_port;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_disconnect_udp(uint32_t owner_pid, uint32_t socket_id)
{
    if (owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    entry->connected = false;
    entry->peer_addr_be = NET_SOCKET_IPV4_ANY;
    entry->peer_port = 0U;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_getsockname_udp(uint32_t owner_pid,
                                uint32_t socket_id,
                                uint32_t* out_local_addr_be,
                                uint16_t* out_local_port)
{
    if (!out_local_addr_be || !out_local_port || owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    *out_local_addr_be = NET_SOCKET_IPV4_ANY;
    *out_local_port = 0U;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    *out_local_addr_be = entry->local_addr_be;
    *out_local_port = entry->local_port;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_getpeername_udp(uint32_t owner_pid,
                                uint32_t socket_id,
                                bool* out_connected,
                                uint32_t* out_peer_addr_be,
                                uint16_t* out_peer_port)
{
    if (!out_connected || !out_peer_addr_be || !out_peer_port || owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    *out_connected = false;
    *out_peer_addr_be = NET_SOCKET_IPV4_ANY;
    *out_peer_port = 0U;

    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    *out_connected = entry->connected;
    *out_peer_addr_be = entry->peer_addr_be;
    *out_peer_port = entry->peer_port;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

bool NET_socket_sendto_udp(uint32_t owner_pid,
                           uint32_t socket_id,
                           uint32_t dst_addr_be,
                           uint16_t dst_port,
                           const uint8_t* payload,
                           size_t payload_len,
                           size_t* out_sent)
{
    if (!out_sent || owner_pid == 0U || !payload || payload_len > NET_SOCKET_UDP_MAX_PAYLOAD || dst_port == 0U)
        return false;

    *out_sent = 0U;
    if (!NET_socket_ensure_initialized())
        return false;

    uint32_t src_addr_be = NET_SOCKET_IPV4_ANY;
    uint16_t src_port = 0U;
    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    if (!NET_socket_auto_bind_locked(entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    src_addr_be = entry->local_addr_be;
    src_port = entry->local_port;
    if (src_addr_be == NET_SOCKET_IPV4_ANY && NET_socket_is_loopback_ipv4(dst_addr_be))
        src_addr_be = NET_SOCKET_IPV4_LOOPBACK_BE;
    spin_unlock(&NET_socket_state.lock);

    if (NET_socket_is_loopback_ipv4(dst_addr_be))
    {
        spin_lock(&NET_socket_state.lock);
        (void) NET_socket_enqueue_udp_locked(dst_addr_be, dst_port, src_addr_be, src_port, payload, payload_len);
        spin_unlock(&NET_socket_state.lock);
        *out_sent = payload_len;
        return true;
    }

    if (!E1000_is_available())
        return false;

    uint8_t src_mac[6];
    if (!E1000_get_mac(src_mac))
        return false;

    uint8_t dst_mac[6];
    if (dst_addr_be == NET_SOCKET_IPV4_BROADCAST)
    {
        memset(dst_mac, 0xFF, sizeof(dst_mac));
    }
    else
    {
        if (!ARP_lookup_ipv4(dst_addr_be, dst_mac, NULL))
            return false;
    }

    size_t udp_len = NET_SOCKET_UDP_HEADER_LEN + payload_len;
    size_t ip_len = NET_SOCKET_IPV4_MIN_HEADER_LEN + udp_len;
    size_t frame_len = NET_SOCKET_ETH_HEADER_LEN + ip_len;
    if (frame_len > NET_SOCKET_ETH_FRAME_MAX_BYTES)
        return false;

    uint8_t frame[NET_SOCKET_ETH_FRAME_MAX_BYTES];
    memset(frame, 0, frame_len);

    memcpy(frame + 0, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    NET_socket_write_be16(frame + 12, NET_SOCKET_ETHERTYPE_IPV4);

    net_socket_ipv4_header_t* ip = (net_socket_ipv4_header_t*) (frame + NET_SOCKET_ETH_HEADER_LEN);
    ip->version_ihl = 0x45U;
    ip->dscp_ecn = 0U;
    ip->total_len_be = 0U;
    NET_socket_write_be16((uint8_t*) &ip->total_len_be, (uint16_t) ip_len);

    uint16_t ip_id = 0U;
    spin_lock(&NET_socket_state.lock);
    ip_id = NET_socket_state.next_ipv4_identification++;
    if (NET_socket_state.next_ipv4_identification == 0U)
        NET_socket_state.next_ipv4_identification = 1U;
    spin_unlock(&NET_socket_state.lock);

    NET_socket_write_be16((uint8_t*) &ip->identification_be, ip_id);
    NET_socket_write_be16((uint8_t*) &ip->flags_frag_be, NET_SOCKET_IPV4_FLAGS_DF);
    ip->ttl = NET_SOCKET_IPV4_TTL_DEFAULT;
    ip->protocol = NET_SOCKET_IPV4_PROTO_UDP;
    ip->checksum_be = 0U;
    NET_socket_write_be32((uint8_t*) &ip->src_addr_be, src_addr_be);
    NET_socket_write_be32((uint8_t*) &ip->dst_addr_be, dst_addr_be);
    uint16_t ip_checksum = NET_socket_checksum_bytes((const uint8_t*) ip, NET_SOCKET_IPV4_MIN_HEADER_LEN);
    NET_socket_write_be16((uint8_t*) &ip->checksum_be, ip_checksum);

    net_socket_udp_header_t* udp = (net_socket_udp_header_t*) (frame + NET_SOCKET_ETH_HEADER_LEN + NET_SOCKET_IPV4_MIN_HEADER_LEN);
    NET_socket_write_be16((uint8_t*) &udp->src_port_be, src_port);
    NET_socket_write_be16((uint8_t*) &udp->dst_port_be, dst_port);
    NET_socket_write_be16((uint8_t*) &udp->len_be, (uint16_t) udp_len);
    udp->checksum_be = 0U;
    memcpy((uint8_t*) (udp + 1), payload, payload_len);
    uint16_t udp_checksum = NET_socket_udp_checksum(src_addr_be, dst_addr_be, (const uint8_t*) udp, udp_len);
    NET_socket_write_be16((uint8_t*) &udp->checksum_be, udp_checksum);

    size_t written = 0U;
    if (!E1000_raw_write(frame, frame_len, &written))
        return false;

    if (written != frame_len)
        return false;

    *out_sent = payload_len;
    return true;
}

bool NET_socket_recvfrom_udp(uint32_t owner_pid,
                             uint32_t socket_id,
                             uint8_t* out_payload,
                             size_t out_cap,
                             size_t* out_len,
                             uint32_t* out_src_addr_be,
                             uint16_t* out_src_port,
                             bool force_non_blocking,
                             bool* out_would_block)
{
    if (!out_payload || !out_len || out_cap == 0U || owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    *out_len = 0U;
    if (out_src_addr_be)
        *out_src_addr_be = 0U;
    if (out_src_port)
        *out_src_port = 0U;
    if (out_would_block)
        *out_would_block = false;

    for (;;)
    {
        net_socket_wait_context_t wait_ctx = { 0 };
        bool should_block = false;
        task_wait_queue_t* waitq = NULL;

        spin_lock(&NET_socket_state.lock);
        net_socket_udp_entry_t* entry = NULL;
        if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
        {
            spin_unlock(&NET_socket_state.lock);
            return false;
        }

        uint32_t queue_count = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
        if (queue_count != 0U)
        {
            net_socket_udp_datagram_t* datagram = &entry->rx_queue[entry->rx_head];
            size_t copy_len = datagram->len;
            if (copy_len > out_cap)
                copy_len = out_cap;

            memcpy(out_payload, datagram->payload, copy_len);
            if (out_src_addr_be)
                *out_src_addr_be = datagram->src.ipv4_addr_be;
            if (out_src_port)
                *out_src_port = datagram->src.port;

            entry->rx_head = (uint16_t) ((entry->rx_head + 1U) % NET_SOCKET_UDP_RX_QUEUE_LEN);
            __atomic_store_n(&entry->rx_count, queue_count - 1U, __ATOMIC_RELEASE);
            spin_unlock(&NET_socket_state.lock);
            *out_len = copy_len;
            return true;
        }

        should_block = !force_non_blocking && !entry->non_blocking && entry->waitq_ready;
        waitq = &entry->rx_waitq;
        wait_ctx.entry = entry;
        spin_unlock(&NET_socket_state.lock);

        if (!should_block)
        {
            if (out_would_block)
                *out_would_block = true;
            return true;
        }

        task_waiter_t waiter = { 0 };
        if (!task_wait_queue_wait_event(waitq,
                                        &waiter,
                                        NET_socket_wait_rx_empty_predicate,
                                        &wait_ctx,
                                        TASK_WAIT_TIMEOUT_INFINITE))
        {
            return false;
        }
    }
}

bool NET_socket_pending_udp_bytes(uint32_t owner_pid, uint32_t socket_id, size_t* out_bytes)
{
    if (!out_bytes || owner_pid == 0U || !NET_socket_ensure_initialized())
        return false;

    *out_bytes = 0U;
    spin_lock(&NET_socket_state.lock);
    net_socket_udp_entry_t* entry = NULL;
    if (!NET_socket_get_udp_entry_locked(owner_pid, socket_id, &entry))
    {
        spin_unlock(&NET_socket_state.lock);
        return false;
    }

    uint32_t queue_count = __atomic_load_n(&entry->rx_count, __ATOMIC_ACQUIRE);
    if (queue_count != 0U)
        *out_bytes = entry->rx_queue[entry->rx_head].len;
    spin_unlock(&NET_socket_state.lock);
    return true;
}

void NET_socket_on_ethernet_frame(const uint8_t* frame, size_t frame_len)
{
    if (!frame || frame_len < (NET_SOCKET_ETH_HEADER_LEN + NET_SOCKET_IPV4_MIN_HEADER_LEN + NET_SOCKET_UDP_HEADER_LEN))
        return;
    if (!NET_socket_ensure_initialized())
        return;

    uint16_t ethertype = NET_socket_read_be16(frame + 12U);
    if (ethertype != NET_SOCKET_ETHERTYPE_IPV4)
        return;

    const net_socket_ipv4_header_t* ip = (const net_socket_ipv4_header_t*) (frame + NET_SOCKET_ETH_HEADER_LEN);
    uint8_t ihl_bytes = (uint8_t) ((ip->version_ihl & 0x0FU) * 4U);
    if ((ip->version_ihl >> 4U) != 4U || ihl_bytes < NET_SOCKET_IPV4_MIN_HEADER_LEN)
        return;

    size_t ip_offset = NET_SOCKET_ETH_HEADER_LEN;
    size_t ip_total_len = NET_socket_read_be16((const uint8_t*) &ip->total_len_be);
    if (ip_total_len < (size_t) ihl_bytes + NET_SOCKET_UDP_HEADER_LEN)
        return;
    if (frame_len < ip_offset + ip_total_len)
        return;

    uint16_t flags_frag = NET_socket_read_be16((const uint8_t*) &ip->flags_frag_be);
    if ((flags_frag & 0x3FFFU) != 0U)
        return;

    if (ip->protocol != NET_SOCKET_IPV4_PROTO_UDP)
        return;

    const uint8_t* udp_ptr = frame + ip_offset + ihl_bytes;
    const net_socket_udp_header_t* udp = (const net_socket_udp_header_t*) udp_ptr;
    uint16_t udp_len = NET_socket_read_be16((const uint8_t*) &udp->len_be);
    if (udp_len < NET_SOCKET_UDP_HEADER_LEN)
        return;
    if ((size_t) udp_len > (ip_total_len - ihl_bytes))
        return;

    uint16_t dst_port = NET_socket_read_be16((const uint8_t*) &udp->dst_port_be);
    uint16_t src_port = NET_socket_read_be16((const uint8_t*) &udp->src_port_be);
    if (dst_port == 0U || src_port == 0U)
        return;

    const uint8_t* payload = udp_ptr + NET_SOCKET_UDP_HEADER_LEN;
    size_t payload_len = udp_len - NET_SOCKET_UDP_HEADER_LEN;
    if (payload_len > NET_SOCKET_UDP_MAX_PAYLOAD)
        return;

    uint32_t dst_addr_be = NET_socket_read_be32((const uint8_t*) &ip->dst_addr_be);
    uint32_t src_addr_be = NET_socket_read_be32((const uint8_t*) &ip->src_addr_be);

    spin_lock(&NET_socket_state.lock);
    (void) NET_socket_enqueue_udp_locked(dst_addr_be, dst_port, src_addr_be, src_port, payload, payload_len);
    spin_unlock(&NET_socket_state.lock);
}
