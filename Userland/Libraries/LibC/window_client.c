#include <window.h>

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <syscall.h>
#include <unistd.h>

#define WS_CLIENT_SEND_CHUNK  1024U
#define WS_CLIENT_SEND_BUDGET 256U
#define WS_CLIENT_RECV_DEADLINE_TICKS 30000ULL

#define WS_CLIENT_LOG(fmt, ...)                                                \
    do                                                                         \
    {                                                                          \
        unsigned long long __tick = (unsigned long long) sys_tick_get();        \
        printf("[window_client t=%llu] " fmt, __tick, ##__VA_ARGS__);          \
        (void) fflush(stdout);                                                 \
    }                                                                          \
    while (0)

static int ws_set_non_blocking(int fd)
{
    if (fd < 0)
    {
        errno = EINVAL;
        return -1;
    }
    int nb = 1;
    return ioctl(fd, FIONBIO, &nb);
}

static void ws_client_reset(ws_client_t* client)
{
    if (!client)
        return;
    memset(client, 0, sizeof(*client));
    client->sock_fd = -1;
    client->role = WS_CLIENT_ROLE_GENERIC;
}

static int ws_send_all(int fd, const void* data, size_t len)
{
    if (fd < 0 || (!data && len != 0U))
    {
        errno = EINVAL;
        return -1;
    }
    const uint8_t* src = (const uint8_t*) data;
    size_t sent = 0U;
    uint32_t budget = 0U;
    while (sent < len)
    {
        size_t chunk = len - sent;
        if (chunk > WS_CLIENT_SEND_CHUNK)
            chunk = WS_CLIENT_SEND_CHUNK;
        ssize_t rc = send(fd, src + sent, chunk, 0);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (budget >= WS_CLIENT_SEND_BUDGET)
                    return -1;
                budget++;
                (void) sched_yield();
                continue;
            }
            return -1;
        }
        if (rc == 0)
        {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t) rc;
    }
    return 0;
}

static int ws_recv_exact(int fd, void* data, size_t len)
{
    if (fd < 0 || (!data && len != 0U))
    {
        errno = EINVAL;
        return -1;
    }
    uint8_t* dst = (uint8_t*) data;
    size_t got = 0U;
    uint64_t deadline = (uint64_t) sys_tick_get() + WS_CLIENT_RECV_DEADLINE_TICKS;
    while (got < len)
    {
        if ((uint64_t) sys_tick_get() >= deadline)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t rc = recv(fd, dst + got, len - got, 0);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                (void) sched_yield();
                continue;
            }
            return -1;
        }
        if (rc == 0)
        {
            errno = ECONNRESET;
            return -1;
        }
        got += (size_t) rc;
    }
    return 0;
}

static int ws_send_header(int fd, uint16_t opcode, uint32_t window_id,
                          const void* payload, uint32_t payload_len)
{
    ws_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WS_IPC_MAGIC;
    hdr.version = WS_IPC_VERSION;
    hdr.opcode = opcode;
    hdr.status = 0;
    hdr.window_id = window_id;
    hdr.payload_len = payload_len;
    if (ws_send_all(fd, &hdr, sizeof(hdr)) < 0)
        return -1;
    if (payload_len > 0U && payload)
    {
        if (ws_send_all(fd, payload, payload_len) < 0)
            return -1;
    }
    return 0;
}

static int ws_recv_response(int fd, ws_ipc_header_t* out_hdr,
                            uint8_t* payload_buf, size_t payload_buf_size,
                            uint32_t* out_payload_len)
{
    ws_ipc_header_t hdr;
    if (ws_recv_exact(fd, &hdr, sizeof(hdr)) < 0)
        return -1;
    if (hdr.magic != WS_IPC_MAGIC || hdr.version != WS_IPC_VERSION)
    {
        WS_CLIENT_LOG("bad response magic=0x%08X version=%u\n",
                      (unsigned int) hdr.magic, (unsigned int) hdr.version);
        errno = EIO;
        return -1;
    }
    uint32_t plen = hdr.payload_len;
    if (plen > WS_IPC_MAX_PAYLOAD_LEN)
    {
        errno = EINVAL;
        return -1;
    }
    if (plen > 0U)
    {
        if (!payload_buf || plen > (uint32_t) payload_buf_size)
        {
            /* Drain payload we can't store */
            uint8_t drain[256];
            uint32_t remain = plen;
            while (remain > 0U)
            {
                uint32_t chunk = remain;
                if (chunk > sizeof(drain))
                    chunk = sizeof(drain);
                if (ws_recv_exact(fd, drain, chunk) < 0)
                    return -1;
                remain -= chunk;
            }
            plen = 0U;
        }
        else
        {
            if (ws_recv_exact(fd, payload_buf, plen) < 0)
                return -1;
        }
    }
    if (out_hdr)
        *out_hdr = hdr;
    if (out_payload_len)
        *out_payload_len = plen;
    return 0;
}

/*
 * Send a request and receive the matching response.  If a POLL_EVENT response
 * arrives while waiting for a different opcode, cache the event and retry.
 */
static int ws_exchange(ws_client_t* client, uint16_t opcode, uint32_t window_id,
                       const void* payload, uint32_t payload_len,
                       ws_ipc_header_t* out_hdr,
                       uint8_t* resp_payload, size_t resp_payload_size,
                       uint32_t* out_resp_payload_len)
{
    if (!client || !client->connected || client->sock_fd < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (ws_send_header(client->sock_fd, opcode, window_id, payload, payload_len) < 0)
    {
        WS_CLIENT_LOG("send failed opcode=%u errno=%d\n", (unsigned int) opcode, errno);
        return -1;
    }

    uint64_t deadline = (uint64_t) sys_tick_get() + WS_CLIENT_RECV_DEADLINE_TICKS;
    for (;;)
    {
        if ((uint64_t) sys_tick_get() >= deadline)
        {
            WS_CLIENT_LOG("recv deadline opcode=%u\n", (unsigned int) opcode);
            errno = ETIMEDOUT;
            return -1;
        }
        ws_ipc_header_t hdr;
        uint32_t rpl = 0U;
        if (ws_recv_response(client->sock_fd, &hdr, resp_payload, resp_payload_size, &rpl) < 0)
        {
            int re = errno;
            if (re == EAGAIN || re == EWOULDBLOCK || re == EINTR)
            {
                (void) sched_yield();
                continue;
            }
            errno = re;
            return -1;
        }

        if (hdr.opcode == opcode)
        {
            if (out_hdr)
                *out_hdr = hdr;
            if (out_resp_payload_len)
                *out_resp_payload_len = rpl;
            return 0;
        }

        /* Cache stray POLL_EVENT response */
        if (hdr.opcode == (uint16_t) WS_IPC_OP_POLL_EVENT)
        {
            if (hdr.status == 0 && rpl >= sizeof(ws_ipc_event_payload_t))
            {
                ws_ipc_event_payload_t ep;
                memcpy(&ep, resp_payload, sizeof(ep));
                client->cached_event.type = (ws_event_type_t) ep.event_type;
                client->cached_event.window_id = hdr.window_id;
                client->cached_event.key = ep.event_key;
                client->poll_event_cached = true;
            }
            continue;
        }

        WS_CLIENT_LOG("unexpected opcode expected=%u got=%u\n",
                      (unsigned int) opcode, (unsigned int) hdr.opcode);
        errno = EIO;
        return -1;
    }
}

static int ws_simple_request(ws_client_t* client, uint16_t opcode, uint32_t window_id,
                             const void* payload, uint32_t payload_len,
                             ws_ipc_header_t* out_hdr,
                             uint8_t* resp_payload, size_t resp_payload_size,
                             uint32_t* out_resp_payload_len)
{
    ws_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    uint32_t rpl = 0U;
    if (ws_exchange(client, opcode, window_id, payload, payload_len,
                    &hdr, resp_payload, resp_payload_size, &rpl) < 0)
        return -1;
    if (hdr.status != 0)
    {
        errno = (hdr.status > 0) ? hdr.status : EIO;
        if (!(opcode == (uint16_t) WS_IPC_OP_POLL_EVENT && errno == EAGAIN))
            WS_CLIENT_LOG("request failed opcode=%u status=%d\n",
                          (unsigned int) opcode, hdr.status);
        return -1;
    }
    if (out_hdr)
        *out_hdr = hdr;
    if (out_resp_payload_len)
        *out_resp_payload_len = rpl;
    return 0;
}

static bool ws_errno_retryable(int err)
{
    return err == EINTR || err == EAGAIN || err == ECONNREFUSED ||
           err == EINVAL || err == EIO || err == EINPROGRESS ||
           err == EALREADY || err == ENOTCONN;
}

int ws_client_connect(ws_client_t* client, ws_client_role_t role)
{
    if (!client)
    {
        errno = EINVAL;
        return -1;
    }
    ws_client_reset(client);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    {
        size_t plen = strlen(WS_SERVER_SOCK_PATH);
        if (plen >= sizeof(addr.sun_path))
            plen = sizeof(addr.sun_path) - 1U;
        memcpy(addr.sun_path, WS_SERVER_SOCK_PATH, plen);
        addr.sun_path[plen] = '\0';
    }
    socklen_t addr_len = (socklen_t)(sizeof(sa_family_t) + strlen(addr.sun_path) + 1U);

    int fd = -1;
    int last_err = ECONNREFUSED;
    bool ok = false;
    for (uint32_t attempt = 0U; attempt < 60U; attempt++)
    {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (const struct sockaddr*) &addr, addr_len) == 0)
        {
            ok = true;
            break;
        }
        last_err = errno;
        (void) close(fd);
        fd = -1;
        if (!ws_errno_retryable(last_err))
            break;
        (void) usleep(100U * 1000U);
    }
    if (!ok)
    {
        errno = last_err ? last_err : ECONNREFUSED;
        WS_CLIENT_LOG("connect failed role=%u errno=%d\n", (unsigned int) role, errno);
        return -1;
    }

    client->sock_fd = fd;
    if (ws_set_non_blocking(client->sock_fd) < 0)
    {
        int se = errno;
        ws_client_disconnect(client);
        errno = se;
        return -1;
    }

    int tid = sys_thread_self();
    client->owner_pid = (tid > 0) ? (uint32_t) tid : 0U;
    client->role = role;
    client->connected = true;

    ws_ipc_hello_payload_t hello;
    hello.owner_pid = client->owner_pid;
    hello.client_role = (uint32_t) role;

    bool hello_ok = false;
    for (uint32_t attempt = 0U; attempt < 80U; attempt++)
    {
        if (ws_simple_request(client, (uint16_t) WS_IPC_OP_HELLO, 0U,
                              &hello, sizeof(hello), NULL, NULL, 0U, NULL) == 0)
        {
            hello_ok = true;
            break;
        }
        if (!ws_errno_retryable(errno))
            break;
        (void) usleep(10U * 1000U);
    }
    if (!hello_ok)
    {
        int se = errno;
        ws_client_disconnect(client);
        errno = se;
        WS_CLIENT_LOG("hello failed role=%u errno=%d\n", (unsigned int) role, errno);
        return -1;
    }

    WS_CLIENT_LOG("connected pid=%u role=%u\n",
                  (unsigned int) client->owner_pid, (unsigned int) role);
    return 0;
}

void ws_client_disconnect(ws_client_t* client)
{
    if (!client)
        return;
    if (client->sock_fd >= 0)
        (void) close(client->sock_fd);
    ws_client_reset(client);
}

static void ws_copy_str(char* out, size_t out_sz, const char* in)
{
    if (!out || out_sz == 0U)
        return;
    out[0] = '\0';
    if (!in)
        return;
    size_t len = 0U;
    size_t max = out_sz - 1U;
    while (len < max && in[len] != '\0')
        len++;
    memcpy(out, in, len);
    out[len] = '\0';
}

int ws_client_create_window(ws_client_t* client, const ws_window_desc_t* desc, uint32_t* out_window_id)
{
    if (!client)
    {
        errno = EINVAL;
        return -1;
    }

    /* Build payload: fixed struct + null-terminated title */
    uint8_t buf[sizeof(ws_ipc_create_window_payload_t) + WS_WINDOW_TITLE_MAX + 1U];
    memset(buf, 0, sizeof(buf));
    ws_ipc_create_window_payload_t* p = (ws_ipc_create_window_payload_t*) buf;
    char* title_dst = (char*) (buf + sizeof(ws_ipc_create_window_payload_t));
    if (desc)
    {
        p->x = desc->x;
        p->y = desc->y;
        p->width = desc->width;
        p->height = desc->height;
        p->color = desc->color;
        p->border_color = desc->border_color;
        p->titlebar_color = desc->titlebar_color;
        p->visible = desc->visible ? 1U : 0U;
        p->frame_controls = desc->frame_controls ? 1U : 0U;
        if (desc->title)
            ws_copy_str(title_dst, WS_WINDOW_TITLE_MAX + 1U, desc->title);
    }
    size_t title_len = 0U;
    while (title_len < WS_WINDOW_TITLE_MAX && title_dst[title_len] != '\0')
        title_len++;
    uint32_t payload_len = (uint32_t) sizeof(ws_ipc_create_window_payload_t) + (uint32_t) title_len + 1U;

    ws_ipc_header_t hdr;
    if (ws_simple_request(client, (uint16_t) WS_IPC_OP_CREATE_WINDOW, 0U,
                          buf, payload_len, &hdr, NULL, 0U, NULL) < 0)
    {
        WS_CLIENT_LOG("create_window failed errno=%d\n", errno);
        return -1;
    }
    if (out_window_id)
        *out_window_id = hdr.window_id;
    return 0;
}

int ws_client_destroy_window(ws_client_t* client, uint32_t window_id)
{
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_DESTROY_WINDOW,
                             window_id, NULL, 0U, NULL, NULL, 0U, NULL);
}

int ws_client_move_window(ws_client_t* client, uint32_t window_id, int32_t x, int32_t y)
{
    ws_ipc_move_payload_t p = { .x = x, .y = y };
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_MOVE_WINDOW,
                             window_id, &p, sizeof(p), NULL, NULL, 0U, NULL);
}

int ws_client_raise_window(ws_client_t* client, uint32_t window_id)
{
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_RAISE_WINDOW,
                             window_id, NULL, 0U, NULL, NULL, 0U, NULL);
}

int ws_client_resize_window(ws_client_t* client, uint32_t window_id, uint32_t width, uint32_t height)
{
    ws_ipc_resize_payload_t p = { .width = width, .height = height };
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_RESIZE_WINDOW,
                             window_id, &p, sizeof(p), NULL, NULL, 0U, NULL);
}

int ws_client_set_window_color(ws_client_t* client, uint32_t window_id, uint32_t color)
{
    ws_ipc_color_payload_t p = { .color = color };
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_SET_WINDOW_COLOR,
                             window_id, &p, sizeof(p), NULL, NULL, 0U, NULL);
}

int ws_client_set_window_visible(ws_client_t* client, uint32_t window_id, bool visible)
{
    ws_ipc_visible_payload_t p = { .visible = visible ? 1U : 0U };
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_SET_WINDOW_VISIBLE,
                             window_id, &p, sizeof(p), NULL, NULL, 0U, NULL);
}

int ws_client_set_window_title(ws_client_t* client, uint32_t window_id, const char* title)
{
    char buf[WS_WINDOW_TITLE_MAX + 1U];
    memset(buf, 0, sizeof(buf));
    ws_copy_str(buf, sizeof(buf), title);
    size_t len = 0U;
    while (len < WS_WINDOW_TITLE_MAX && buf[len] != '\0')
        len++;
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_SET_WINDOW_TITLE,
                             window_id, buf, (uint32_t) len + 1U, NULL, NULL, 0U, NULL);
}

int ws_client_set_window_text(ws_client_t* client, uint32_t window_id, const char* text)
{
    char buf[WS_WINDOW_BODY_TEXT_MAX + 1U];
    memset(buf, 0, sizeof(buf));
    ws_copy_str(buf, sizeof(buf), text);
    size_t len = 0U;
    while (len < WS_WINDOW_BODY_TEXT_MAX && buf[len] != '\0')
        len++;
    return ws_simple_request(client, (uint16_t) WS_IPC_OP_SET_WINDOW_TEXT,
                             window_id, buf, (uint32_t) len + 1U, NULL, NULL, 0U, NULL);
}

int ws_client_poll_event(ws_client_t* client, ws_event_t* out_event)
{
    if (!client || !out_event)
    {
        errno = EINVAL;
        return -1;
    }

    if (client->poll_event_cached)
    {
        *out_event = client->cached_event;
        client->poll_event_cached = false;
        return 0;
    }

    ws_ipc_header_t hdr;
    uint8_t rbuf[sizeof(ws_ipc_event_payload_t)];
    uint32_t rpl = 0U;
    int rc = ws_exchange(client, (uint16_t) WS_IPC_OP_POLL_EVENT, 0U,
                         NULL, 0U, &hdr, rbuf, sizeof(rbuf), &rpl);
    if (rc < 0)
    {
        if (errno == EWOULDBLOCK)
            errno = EAGAIN;
        return -1;
    }
    if (hdr.status != 0)
    {
        errno = (hdr.status > 0) ? hdr.status : EIO;
        return -1;
    }
    if (rpl < sizeof(ws_ipc_event_payload_t))
    {
        errno = EIO;
        return -1;
    }
    ws_ipc_event_payload_t ep;
    memcpy(&ep, rbuf, sizeof(ep));
    out_event->type = (ws_event_type_t) ep.event_type;
    out_event->window_id = hdr.window_id;
    out_event->key = ep.event_key;
    return 0;
}
