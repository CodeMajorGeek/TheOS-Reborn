#include <Network/Unix.h>
#include <Debug/Logger.h>
#include <string.h>

static net_unix_state_t NET_unix_state;

void NET_unix_init(void)
{
    memset(&NET_unix_state, 0, sizeof(NET_unix_state));
    spinlock_init(&NET_unix_state.lock);
    NET_unix_state.lock_ready = true;
    NET_unix_state.initialized = true;
}

bool NET_unix_is_ready(void)
{
    return NET_unix_state.initialized && NET_unix_state.lock_ready;
}

/* ------------------------------------------------------------------ */
/*  Internal helpers (caller holds NET_unix_state.lock)                */
/* ------------------------------------------------------------------ */

static net_unix_entry_t* NET_unix_get_entry_locked(uint32_t socket_id)
{
    if (socket_id >= NET_UNIX_MAX_SOCKETS)
        return NULL;
    net_unix_entry_t* e = &NET_unix_state.entries[socket_id];
    return e->used ? e : NULL;
}

static net_unix_entry_t* NET_unix_get_owned_locked(uint32_t owner_pid, uint32_t socket_id)
{
    net_unix_entry_t* e = NET_unix_get_entry_locked(socket_id);
    if (!e || e->owner_pid != owner_pid)
        return NULL;
    return e;
}

static int32_t NET_unix_find_by_path_locked(const char* path)
{
    for (uint32_t i = 0; i < NET_UNIX_MAX_SOCKETS; i++)
    {
        net_unix_entry_t* e = &NET_unix_state.entries[i];
        if (e->used && e->bound && strcmp(e->path, path) == 0)
            return (int32_t) i;
    }
    return -1;
}

static void NET_unix_init_waitqs(net_unix_entry_t* e)
{
    task_wait_queue_init(&e->rx_waitq);
    task_wait_queue_init(&e->accept_waitq);
    task_wait_queue_init(&e->state_waitq);
    e->waitq_ready = true;
}

/* ------------------------------------------------------------------ */
/*  Wait predicates                                                    */
/* ------------------------------------------------------------------ */

typedef struct net_unix_wait_ctx
{
    net_unix_entry_t* entry;
} net_unix_wait_ctx_t;

static bool NET_unix_rx_empty_predicate(void* ctx)
{
    net_unix_wait_ctx_t* wc = (net_unix_wait_ctx_t*) ctx;
    if (!wc || !wc->entry)
        return false;
    if (wc->entry->socket_type == NET_UNIX_SOCK_DGRAM)
        return __atomic_load_n(&wc->entry->dgram_rx_count, __ATOMIC_ACQUIRE) == 0U &&
               !wc->entry->peer_closed;
    return __atomic_load_n(&wc->entry->stream_rx_count, __ATOMIC_ACQUIRE) == 0U &&
           !wc->entry->peer_closed;
}

static bool NET_unix_accept_empty_predicate(void* ctx)
{
    net_unix_wait_ctx_t* wc = (net_unix_wait_ctx_t*) ctx;
    if (!wc || !wc->entry)
        return false;
    return __atomic_load_n(&wc->entry->accept_count, __ATOMIC_ACQUIRE) == 0U;
}

static bool NET_unix_connect_pending_predicate(void* ctx)
{
    net_unix_wait_ctx_t* wc = (net_unix_wait_ctx_t*) ctx;
    if (!wc || !wc->entry)
        return false;
    return !wc->entry->connected && !wc->entry->peer_closed;
}

/* ------------------------------------------------------------------ */
/*  Create / Close                                                     */
/* ------------------------------------------------------------------ */

bool NET_unix_create(uint32_t owner_pid, uint32_t socket_type, uint32_t* out_socket_id)
{
    if (!out_socket_id || owner_pid == 0U)
        return false;
    if (socket_type != NET_UNIX_SOCK_STREAM && socket_type != NET_UNIX_SOCK_DGRAM)
        return false;

    spin_lock(&NET_unix_state.lock);
    for (uint32_t i = 0; i < NET_UNIX_MAX_SOCKETS; i++)
    {
        net_unix_entry_t* e = &NET_unix_state.entries[i];
        if (e->used)
            continue;

        memset(e, 0, sizeof(*e));
        e->used = true;
        e->owner_pid = owner_pid;
        e->socket_type = socket_type;
        NET_unix_init_waitqs(e);
        *out_socket_id = i;
        spin_unlock(&NET_unix_state.lock);
        return true;
    }
    spin_unlock(&NET_unix_state.lock);
    return false;
}

bool NET_unix_close(uint32_t owner_pid, uint32_t socket_id)
{
    if (owner_pid == 0U)
        return false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (e->connected && e->peer_id < NET_UNIX_MAX_SOCKETS)
    {
        net_unix_entry_t* peer = NET_unix_get_entry_locked(e->peer_id);
        if (peer && peer->peer_id == socket_id)
        {
            peer->peer_closed = true;
            peer->connected = false;
            if (peer->waitq_ready)
            {
                task_wait_queue_wake_all(&peer->rx_waitq);
                task_wait_queue_wake_all(&peer->state_waitq);
            }
        }
    }

    bool had_waitq = e->waitq_ready;
    task_wait_queue_t saved_rx = e->rx_waitq;
    task_wait_queue_t saved_accept = e->accept_waitq;
    task_wait_queue_t saved_state = e->state_waitq;
    memset(e, 0, sizeof(*e));
    spin_unlock(&NET_unix_state.lock);

    if (had_waitq)
    {
        task_wait_queue_wake_all(&saved_rx);
        task_wait_queue_wake_all(&saved_accept);
        task_wait_queue_wake_all(&saved_state);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Bind                                                               */
/* ------------------------------------------------------------------ */

bool NET_unix_bind(uint32_t owner_pid, uint32_t socket_id, const char* path, size_t path_len)
{
    if (!path || path_len == 0 || path_len >= NET_UNIX_PATH_MAX || owner_pid == 0U)
        return false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e || e->bound)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (NET_unix_find_by_path_locked(path) >= 0)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    memcpy(e->path, path, path_len);
    e->path[path_len] = '\0';
    e->bound = true;
    spin_unlock(&NET_unix_state.lock);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Connect                                                            */
/* ------------------------------------------------------------------ */

bool NET_unix_connect(uint32_t owner_pid, uint32_t socket_id, const char* path, size_t path_len,
                      bool force_non_blocking, bool* out_would_block)
{
    if (!path || path_len == 0 || path_len >= NET_UNIX_PATH_MAX || owner_pid == 0U)
        return false;
    if (out_would_block)
        *out_would_block = false;

    char safe_path[NET_UNIX_PATH_MAX];
    memcpy(safe_path, path, path_len);
    safe_path[path_len] = '\0';

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    int32_t target = NET_unix_find_by_path_locked(safe_path);
    if (target < 0)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }
    net_unix_entry_t* target_entry = &NET_unix_state.entries[(uint32_t) target];

    if (e->socket_type == NET_UNIX_SOCK_DGRAM)
    {
        e->peer_id = (uint32_t) target;
        e->connected = true;
        spin_unlock(&NET_unix_state.lock);
        return true;
    }

    /* SOCK_STREAM: target must be listening */
    if (!target_entry->listening || target_entry->socket_type != NET_UNIX_SOCK_STREAM)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (target_entry->accept_count >= NET_UNIX_ACCEPT_QUEUE_LEN)
    {
        spin_unlock(&NET_unix_state.lock);
        if (out_would_block)
            *out_would_block = true;
        return !force_non_blocking;
    }

    /* Create server-side socket for the accepted connection */
    uint32_t server_sock_id = NET_UNIX_MAX_SOCKETS;
    for (uint32_t i = 0; i < NET_UNIX_MAX_SOCKETS; i++)
    {
        if (!NET_unix_state.entries[i].used)
        {
            server_sock_id = i;
            break;
        }
    }
    if (server_sock_id >= NET_UNIX_MAX_SOCKETS)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    net_unix_entry_t* server = &NET_unix_state.entries[server_sock_id];
    memset(server, 0, sizeof(*server));
    server->used = true;
    server->owner_pid = target_entry->owner_pid;
    server->socket_type = NET_UNIX_SOCK_STREAM;
    server->connected = true;
    server->peer_id = socket_id;
    NET_unix_init_waitqs(server);

    e->peer_id = server_sock_id;
    e->connected = true;

    target_entry->accept_queue[target_entry->accept_tail] = server_sock_id;
    target_entry->accept_tail = (uint16_t) ((target_entry->accept_tail + 1U) % NET_UNIX_ACCEPT_QUEUE_LEN);
    __atomic_store_n(&target_entry->accept_count, target_entry->accept_count + 1U, __ATOMIC_RELEASE);

    if (target_entry->waitq_ready)
        task_wait_queue_wake_all(&target_entry->accept_waitq);

    spin_unlock(&NET_unix_state.lock);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Listen / Accept                                                    */
/* ------------------------------------------------------------------ */

bool NET_unix_listen(uint32_t owner_pid, uint32_t socket_id, uint32_t backlog)
{
    if (owner_pid == 0U)
        return false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e || !e->bound || e->socket_type != NET_UNIX_SOCK_STREAM || e->listening)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    e->listening = true;
    e->listen_backlog = (uint16_t) (backlog < NET_UNIX_ACCEPT_QUEUE_LEN ? backlog : NET_UNIX_ACCEPT_QUEUE_LEN);
    spin_unlock(&NET_unix_state.lock);
    return true;
}

bool NET_unix_accept(uint32_t owner_pid, uint32_t socket_id, uint32_t* out_child_socket_id,
                     bool force_non_blocking, bool* out_would_block)
{
    if (!out_child_socket_id || owner_pid == 0U)
        return false;
    if (out_would_block)
        *out_would_block = false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e || !e->listening)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (e->accept_count == 0U)
    {
        if (force_non_blocking || e->non_blocking)
        {
            spin_unlock(&NET_unix_state.lock);
            if (out_would_block)
                *out_would_block = true;
            return true;
        }

        net_unix_wait_ctx_t wc = { .entry = e };
        spin_unlock(&NET_unix_state.lock);

        task_waiter_t waiter;
        task_waiter_init(&waiter);
        task_wait_queue_wait_event(&e->accept_waitq, &waiter,
                                   NET_unix_accept_empty_predicate, &wc,
                                   TASK_WAIT_TIMEOUT_INFINITE);

        spin_lock(&NET_unix_state.lock);
        e = NET_unix_get_owned_locked(owner_pid, socket_id);
        if (!e || !e->listening || e->accept_count == 0U)
        {
            spin_unlock(&NET_unix_state.lock);
            return false;
        }
    }

    uint32_t child_id = e->accept_queue[e->accept_head];
    e->accept_head = (uint16_t) ((e->accept_head + 1U) % NET_UNIX_ACCEPT_QUEUE_LEN);
    __atomic_store_n(&e->accept_count, e->accept_count - 1U, __ATOMIC_RELEASE);

    net_unix_entry_t* child = NET_unix_get_entry_locked(child_id);
    if (child)
        child->owner_pid = owner_pid;

    *out_child_socket_id = child_id;
    spin_unlock(&NET_unix_state.lock);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Sendto (DGRAM: by path or connected peer, STREAM: write to peer)   */
/* ------------------------------------------------------------------ */

bool NET_unix_sendto(uint32_t owner_pid, uint32_t socket_id,
                     const uint8_t* payload, size_t payload_len,
                     const char* dest_path, size_t dest_path_len,
                     size_t* out_sent)
{
    if (!payload || payload_len == 0 || !out_sent || owner_pid == 0U)
        return false;
    *out_sent = 0;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (e->socket_type == NET_UNIX_SOCK_DGRAM)
    {
        if (payload_len > NET_UNIX_RX_MAX_PAYLOAD)
        {
            spin_unlock(&NET_unix_state.lock);
            return false;
        }

        net_unix_entry_t* target = NULL;
        if (dest_path && dest_path_len > 0)
        {
            char safe_path[NET_UNIX_PATH_MAX];
            if (dest_path_len >= NET_UNIX_PATH_MAX)
            {
                spin_unlock(&NET_unix_state.lock);
                return false;
            }
            memcpy(safe_path, dest_path, dest_path_len);
            safe_path[dest_path_len] = '\0';
            int32_t tid = NET_unix_find_by_path_locked(safe_path);
            if (tid < 0)
            {
                spin_unlock(&NET_unix_state.lock);
                return false;
            }
            target = &NET_unix_state.entries[(uint32_t) tid];
        }
        else if (e->connected)
        {
            target = NET_unix_get_entry_locked(e->peer_id);
        }

        if (!target || target->socket_type != NET_UNIX_SOCK_DGRAM)
        {
            spin_unlock(&NET_unix_state.lock);
            return false;
        }

        if (target->dgram_rx_count >= NET_UNIX_RX_QUEUE_LEN)
        {
            spin_unlock(&NET_unix_state.lock);
            return false;
        }

        net_unix_datagram_t* slot = &target->dgram_rx_queue[target->dgram_rx_tail];
        slot->len = (uint16_t) payload_len;
        slot->src_socket_id = socket_id;
        memcpy(slot->payload, payload, payload_len);
        target->dgram_rx_tail = (uint16_t) ((target->dgram_rx_tail + 1U) % NET_UNIX_RX_QUEUE_LEN);
        __atomic_store_n(&target->dgram_rx_count, target->dgram_rx_count + 1U, __ATOMIC_RELEASE);

        if (target->waitq_ready)
            task_wait_queue_wake_all(&target->rx_waitq);

        *out_sent = payload_len;
        spin_unlock(&NET_unix_state.lock);
        return true;
    }

    /* SOCK_STREAM */
    if (!e->connected || e->peer_closed)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    net_unix_entry_t* peer = NET_unix_get_entry_locked(e->peer_id);
    if (!peer)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    uint32_t avail = NET_UNIX_STREAM_BUF_BYTES - peer->stream_rx_count;
    size_t to_write = payload_len < avail ? payload_len : avail;
    if (to_write == 0)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    for (size_t i = 0; i < to_write; i++)
    {
        peer->stream_rx_buf[peer->stream_rx_tail] = payload[i];
        peer->stream_rx_tail = (uint16_t) ((peer->stream_rx_tail + 1U) % NET_UNIX_STREAM_BUF_BYTES);
    }
    __atomic_store_n(&peer->stream_rx_count, peer->stream_rx_count + (uint32_t) to_write, __ATOMIC_RELEASE);

    if (peer->waitq_ready)
        task_wait_queue_wake_all(&peer->rx_waitq);

    *out_sent = to_write;
    spin_unlock(&NET_unix_state.lock);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Recvfrom                                                           */
/* ------------------------------------------------------------------ */

bool NET_unix_recvfrom(uint32_t owner_pid, uint32_t socket_id,
                       uint8_t* out_payload, size_t out_cap, size_t* out_len,
                       bool force_non_blocking, bool* out_would_block)
{
    if (!out_payload || !out_len || owner_pid == 0U)
        return false;
    *out_len = 0;
    if (out_would_block)
        *out_would_block = false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }

    if (e->socket_type == NET_UNIX_SOCK_DGRAM)
    {
        if (e->dgram_rx_count == 0U)
        {
            if (force_non_blocking || e->non_blocking)
            {
                spin_unlock(&NET_unix_state.lock);
                if (out_would_block)
                    *out_would_block = true;
                return false;
            }

            net_unix_wait_ctx_t wc = { .entry = e };
            spin_unlock(&NET_unix_state.lock);

            task_waiter_t waiter;
            task_waiter_init(&waiter);
            task_wait_queue_wait_event(&e->rx_waitq, &waiter,
                                       NET_unix_rx_empty_predicate, &wc,
                                       TASK_WAIT_TIMEOUT_INFINITE);

            spin_lock(&NET_unix_state.lock);
            e = NET_unix_get_owned_locked(owner_pid, socket_id);
            if (!e)
            {
                spin_unlock(&NET_unix_state.lock);
                return false;
            }
            if (e->dgram_rx_count == 0U)
            {
                spin_unlock(&NET_unix_state.lock);
                *out_len = 0;
                return true;
            }
        }

        net_unix_datagram_t* slot = &e->dgram_rx_queue[e->dgram_rx_head];
        size_t copy_len = slot->len < out_cap ? slot->len : out_cap;
        memcpy(out_payload, slot->payload, copy_len);
        *out_len = copy_len;
        e->dgram_rx_head = (uint16_t) ((e->dgram_rx_head + 1U) % NET_UNIX_RX_QUEUE_LEN);
        __atomic_store_n(&e->dgram_rx_count, e->dgram_rx_count - 1U, __ATOMIC_RELEASE);
        spin_unlock(&NET_unix_state.lock);
        return true;
    }

    /* SOCK_STREAM */
    if (e->stream_rx_count == 0U)
    {
        if (e->peer_closed)
        {
            spin_unlock(&NET_unix_state.lock);
            *out_len = 0;
            return true;
        }

        if (force_non_blocking || e->non_blocking)
        {
            spin_unlock(&NET_unix_state.lock);
            if (out_would_block)
                *out_would_block = true;
            return false;
        }

        net_unix_wait_ctx_t wc = { .entry = e };
        spin_unlock(&NET_unix_state.lock);

        task_waiter_t waiter;
        task_waiter_init(&waiter);
        task_wait_queue_wait_event(&e->rx_waitq, &waiter,
                                   NET_unix_rx_empty_predicate, &wc,
                                   TASK_WAIT_TIMEOUT_INFINITE);

        spin_lock(&NET_unix_state.lock);
        e = NET_unix_get_owned_locked(owner_pid, socket_id);
        if (!e)
        {
            spin_unlock(&NET_unix_state.lock);
            return false;
        }
        if (e->stream_rx_count == 0U)
        {
            spin_unlock(&NET_unix_state.lock);
            *out_len = 0;
            return true;
        }
    }

    size_t to_read = e->stream_rx_count < out_cap ? e->stream_rx_count : out_cap;
    for (size_t i = 0; i < to_read; i++)
    {
        out_payload[i] = e->stream_rx_buf[e->stream_rx_head];
        e->stream_rx_head = (uint16_t) ((e->stream_rx_head + 1U) % NET_UNIX_STREAM_BUF_BYTES);
    }
    __atomic_store_n(&e->stream_rx_count, e->stream_rx_count - (uint32_t) to_read, __ATOMIC_RELEASE);
    *out_len = to_read;
    spin_unlock(&NET_unix_state.lock);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Non-blocking mode                                                  */
/* ------------------------------------------------------------------ */

bool NET_unix_set_non_blocking(uint32_t owner_pid, uint32_t socket_id,
                               bool non_blocking, bool* out_non_blocking)
{
    if (owner_pid == 0U)
        return false;

    spin_lock(&NET_unix_state.lock);
    net_unix_entry_t* e = NET_unix_get_owned_locked(owner_pid, socket_id);
    if (!e)
    {
        spin_unlock(&NET_unix_state.lock);
        return false;
    }
    e->non_blocking = non_blocking;
    if (out_non_blocking)
        *out_non_blocking = non_blocking;
    spin_unlock(&NET_unix_state.lock);
    return true;
}
