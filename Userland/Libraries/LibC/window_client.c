#include <window.h>

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <arpa/inet.h>
#include <syscall.h>
#include <unistd.h>

#define WS_IPC_SEND_CHUNK_BYTES 1024U
#define WS_CLIENT_LOG(fmt, ...)                                                                                              \
    do                                                                                                                       \
    {                                                                                                                        \
        unsigned long long __tick = (unsigned long long) sys_tick_get();                                                    \
        printf("[window_client t=%llu] " fmt, __tick, ##__VA_ARGS__);                                                      \
    }                                                                                                                        \
    while (0)

static int ws_set_socket_non_blocking(int fd)
{
    if (fd < 0)
    {
        errno = EINVAL;
        return -1;
    }

    int non_blocking = 1;
    if (ioctl(fd, FIONBIO, &non_blocking) < 0)
        return -1;

    return 0;
}

static void ws_client_reset(ws_client_t* client)
{
    if (!client)
        return;

    memset(client, 0, sizeof(*client));
    client->sock_fd = -1;
    client->role = WS_CLIENT_ROLE_GENERIC;
}

static void ws_copy_string_limit(char* out, size_t out_size, const char* in)
{
    if (!out || out_size == 0U)
        return;

    out[0] = '\0';
    if (!in || in[0] == '\0')
        return;

    size_t max_copy = out_size - 1U;
    size_t len = 0U;
    while (len < max_copy && in[len] != '\0')
        len++;
    memcpy(out, in, len);
    out[len] = '\0';
}

static int ws_send_all_with_retry_budget(int fd, const void* data, size_t len, uint32_t retry_budget_limit)
{
    if (fd < 0 || (!data && len != 0U))
    {
        errno = EINVAL;
        return -1;
    }

    const uint8_t* src = (const uint8_t*) data;
    size_t sent = 0U;
    uint32_t retry_budget = 0U;
    while (sent < len)
    {
        size_t remaining = len - sent;
        size_t chunk = remaining;
        if (chunk > WS_IPC_SEND_CHUNK_BYTES)
            chunk = WS_IPC_SEND_CHUNK_BYTES;

        ssize_t rc = send(fd, src + sent, chunk, 0);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (retry_budget >= retry_budget_limit)
                    return -1;
                retry_budget++;
                (void) usleep(1000U);
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

static int ws_send_all(int fd, const void* data, size_t len)
{
    return ws_send_all_with_retry_budget(fd, data, len, 256U);
}

static int ws_recv_all_with_retry_budget(int fd, void* data, size_t len, uint32_t retry_budget_limit)
{
    if (fd < 0 || (!data && len != 0U))
    {
        errno = EINVAL;
        return -1;
    }

    uint8_t* dst = (uint8_t*) data;
    size_t received = 0U;
    uint32_t retry_budget = 0U;
    while (received < len)
    {
        ssize_t rc = recv(fd, dst + received, len - received, 0);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (retry_budget >= retry_budget_limit)
                    return -1;
                retry_budget++;
                (void) usleep(1000U);
                continue;
            }
            return -1;
        }
        if (rc == 0)
        {
            errno = ECONNRESET;
            return -1;
        }
        received += (size_t) rc;
    }

    return 0;
}

static int ws_recv_all(int fd, void* data, size_t len)
{
    return ws_recv_all_with_retry_budget(fd, data, len, 256U);
}

static int ws_client_cache_poll_event_if_any(ws_client_t* client)
{
    if (!client || !client->poll_event_inflight)
        return 0;

    ws_ipc_packet_t response;
    if (ws_recv_all_with_retry_budget(client->sock_fd, &response, sizeof(response), 0U) < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return 0;
        return -1;
    }

    client->poll_event_inflight = false;
    if (response.magic != WS_IPC_MAGIC || response.version != WS_IPC_VERSION)
    {
        errno = EIO;
        return -1;
    }

    if (response.status == 0)
    {
        client->cached_event.type = (ws_event_type_t) response.event_type;
        client->cached_event.window_id = response.window_id;
        client->cached_event.key = response.event_key;
        client->poll_event_cached = true;
    }

    return 0;
}

static int ws_client_exchange_with_retry_budget(ws_client_t* client,
                                                const ws_ipc_packet_t* request,
                                                ws_ipc_packet_t* response,
                                                uint32_t send_retry_budget_limit,
                                                uint32_t recv_retry_budget_limit)
{
    if (!client || !client->connected || client->sock_fd < 0 || !request || !response)
    {
        errno = EINVAL;
        return -1;
    }

    if (ws_send_all_with_retry_budget(client->sock_fd, request, sizeof(*request), send_retry_budget_limit) < 0)
    {
        WS_CLIENT_LOG("send failed opcode=%u errno=%d\n",
                      (unsigned int) request->opcode,
                      errno);
        return -1;
    }
    for (uint32_t recv_attempt = 0U; recv_attempt < 8U; recv_attempt++)
    {
        if (ws_recv_all_with_retry_budget(client->sock_fd, response, sizeof(*response), recv_retry_budget_limit) < 0)
        {
            WS_CLIENT_LOG("recv failed opcode=%u errno=%d\n",
                          (unsigned int) request->opcode,
                          errno);
            return -1;
        }

        if (response->magic != WS_IPC_MAGIC || response->version != WS_IPC_VERSION)
        {
            WS_CLIENT_LOG("bad response header opcode=%u magic=0x%08X version=%u\n",
                          (unsigned int) request->opcode,
                          (unsigned int) response->magic,
                          (unsigned int) response->version);
            errno = EIO;
            return -1;
        }

        if (response->opcode == request->opcode)
            return 0;

        if (response->opcode == (uint16_t) WS_IPC_OP_POLL_EVENT)
        {
            client->poll_event_inflight = false;
            if (response->status == 0)
            {
                client->cached_event.type = (ws_event_type_t) response->event_type;
                client->cached_event.window_id = response->window_id;
                client->cached_event.key = response->event_key;
                client->poll_event_cached = true;
            }
            WS_CLIENT_LOG("[AGENTDBG H32 IPC_REORDER] expected=%u got=%u status=%d cached=%u\n",
                          (unsigned int) request->opcode,
                          (unsigned int) response->opcode,
                          response->status,
                          client->poll_event_cached ? 1U : 0U);
            continue;
        }

        WS_CLIENT_LOG("unexpected response opcode expected=%u got=%u status=%d\n",
                      (unsigned int) request->opcode,
                      (unsigned int) response->opcode,
                      response->status);
        errno = EIO;
        return -1;
    }

    errno = EIO;
    return -1;
}

static int ws_client_exchange(ws_client_t* client,
                              const ws_ipc_packet_t* request,
                              ws_ipc_packet_t* response)
{
    return ws_client_exchange_with_retry_budget(client, request, response, 256U, 256U);
}

static int ws_client_request(ws_client_t* client,
                             ws_ipc_opcode_t opcode,
                             const ws_ipc_packet_t* request_payload,
                             ws_ipc_packet_t* out_response)
{
    if (!client)
    {
        errno = EINVAL;
        return -1;
    }

    if (opcode != WS_IPC_OP_POLL_EVENT)
    {
        if (ws_client_cache_poll_event_if_any(client) < 0)
            return -1;
    }

    ws_ipc_packet_t request;
    memset(&request, 0, sizeof(request));
    request.magic = WS_IPC_MAGIC;
    request.version = WS_IPC_VERSION;
    request.opcode = (uint16_t) opcode;

    if (request_payload)
    {
        request.window_id = request_payload->window_id;
        request.x = request_payload->x;
        request.y = request_payload->y;
        request.width = request_payload->width;
        request.height = request_payload->height;
        request.color = request_payload->color;
        request.border_color = request_payload->border_color;
        request.titlebar_color = request_payload->titlebar_color;
        request.visible = request_payload->visible;
        request.frame_controls = request_payload->frame_controls;
        request.owner_pid = request_payload->owner_pid;
        request.client_role = request_payload->client_role;
        request.event_type = request_payload->event_type;
        request.event_key = request_payload->event_key;
        ws_copy_string_limit(request.title, sizeof(request.title), request_payload->title);
        ws_copy_string_limit(request.text, sizeof(request.text), request_payload->text);
    }

    ws_ipc_packet_t response;
    memset(&response, 0, sizeof(response));
    uint64_t request_start_tick = sys_tick_get();
    if (ws_client_exchange(client, &request, &response) < 0)
    {
        if (opcode == WS_IPC_OP_SET_WINDOW_TEXT)
        {
            uint64_t request_ticks = sys_tick_get() - request_start_tick;
            // #region agent log
            WS_CLIENT_LOG("[AGENTDBG H29 SETTEXT_EXCHANGE_FAIL] ticks=%llu errno=%d\n",
                          (unsigned long long) request_ticks,
                          errno);
            // #endregion
        }
        return -1;
    }
    if (opcode == WS_IPC_OP_SET_WINDOW_TEXT)
    {
        uint64_t request_ticks = sys_tick_get() - request_start_tick;
        if (request_ticks > 20ULL)
        {
            // #region agent log
            WS_CLIENT_LOG("[AGENTDBG H29 SETTEXT_EXCHANGE_SLOW] ticks=%llu status=%d\n",
                          (unsigned long long) request_ticks,
                          response.status);
            // #endregion
        }
    }

    if (response.status != 0)
    {
        errno = (response.status > 0) ? response.status : EIO;
        if (!(opcode == WS_IPC_OP_POLL_EVENT && errno == EAGAIN))
        {
            WS_CLIENT_LOG("request failed opcode=%u status=%d errno=%d win=%u\n",
                          (unsigned int) opcode,
                          response.status,
                          errno,
                          (unsigned int) response.window_id);
        }
        return -1;
    }

    if (out_response)
        *out_response = response;
    return 0;
}

static bool ws_errno_is_connect_retryable(int err)
{
    return err == EINTR ||
           err == EAGAIN ||
           err == ECONNREFUSED ||
           err == EINVAL ||
           err == EIO ||
           err == EINPROGRESS ||
           err == EALREADY ||
           err == ENOTCONN;
}

int ws_client_connect(ws_client_t* client, ws_client_role_t role)
{
    if (!client)
    {
        errno = EINVAL;
        return -1;
    }

    ws_client_reset(client);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) WS_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int fd = -1;
    int connect_errno = ECONNREFUSED;
    bool connected = false;
    for (uint32_t attempt = 0U; attempt < 60U; attempt++)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        if (connect(fd, (const struct sockaddr*) &addr, (socklen_t) sizeof(addr)) == 0)
        {
            connected = true;
            WS_CLIENT_LOG("connect success attempt=%u fd=%d role=%u\n",
                          (unsigned int) (attempt + 1U),
                          fd,
                          (unsigned int) role);
            break;
        }

        connect_errno = errno;
        (void) close(fd);
        fd = -1;

        if (!ws_errno_is_connect_retryable(connect_errno))
            break;
        (void) usleep(100U * 1000U);
    }

    if (!connected)
    {
        errno = connect_errno ? connect_errno : ECONNREFUSED;
        WS_CLIENT_LOG("connect failed role=%u errno=%d\n",
                      (unsigned int) role,
                      errno);
        return -1;
    }

    client->sock_fd = fd;
    if (ws_set_socket_non_blocking(client->sock_fd) < 0)
    {
        int saved_errno = errno;
        ws_client_disconnect(client);
        errno = saved_errno;
        WS_CLIENT_LOG("set-nonblock failed role=%u errno=%d\n",
                      (unsigned int) role,
                      errno);
        return -1;
    }

    int self_tid = sys_thread_self();
    client->owner_pid = (self_tid > 0) ? (uint32_t) self_tid : 0U;
    client->role = role;
    client->connected = true;

    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_pid = client->owner_pid;
    payload.client_role = (uint32_t) role;

    bool hello_ok = false;
    for (uint32_t attempt = 0U; attempt < 80U; attempt++)
    {
        if (ws_client_request(client, WS_IPC_OP_HELLO, &payload, NULL) == 0)
        {
            hello_ok = true;
            break;
        }

        if (!ws_errno_is_connect_retryable(errno))
            break;
        (void) usleep(10U * 1000U);
    }

    if (!hello_ok)
    {
        int saved_errno = errno;
        ws_client_disconnect(client);
        errno = saved_errno;
        WS_CLIENT_LOG("hello failed role=%u errno=%d\n",
                      (unsigned int) role,
                      errno);
        return -1;
    }

    WS_CLIENT_LOG("hello ok pid=%u role=%u\n",
                  (unsigned int) client->owner_pid,
                  (unsigned int) role);
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

int ws_client_create_window(ws_client_t* client, const ws_window_desc_t* desc, uint32_t* out_window_id)
{
    if (!client)
    {
        errno = EINVAL;
        return -1;
    }

    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    char safe_title[WS_WINDOW_TITLE_MAX + 1U];
    safe_title[0] = '\0';
    if (desc)
    {
        payload.x = desc->x;
        payload.y = desc->y;
        payload.width = desc->width;
        payload.height = desc->height;
        payload.color = desc->color;
        payload.border_color = desc->border_color;
        payload.titlebar_color = desc->titlebar_color;
        payload.visible = desc->visible ? 1U : 0U;
        payload.frame_controls = desc->frame_controls ? 1U : 0U;
        ws_copy_string_limit(payload.title, sizeof(payload.title), desc->title);
    }
    ws_copy_string_limit(safe_title, sizeof(safe_title), payload.title);

    ws_ipc_packet_t response;
    if (ws_client_request(client, WS_IPC_OP_CREATE_WINDOW, &payload, &response) < 0)
    {
        WS_CLIENT_LOG("create-window failed errno=%d x=%d y=%d w=%u h=%u title='%s'\n",
                      errno,
                      payload.x,
                      payload.y,
                      (unsigned int) payload.width,
                      (unsigned int) payload.height,
                      safe_title);
        return -1;
    }

    if (out_window_id)
        *out_window_id = response.window_id;
    WS_CLIENT_LOG("create-window ok win=%u title='%s'\n",
                  (unsigned int) response.window_id,
                  safe_title);
    return 0;
}

int ws_client_destroy_window(ws_client_t* client, uint32_t window_id)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    return ws_client_request(client, WS_IPC_OP_DESTROY_WINDOW, &payload, NULL);
}

int ws_client_move_window(ws_client_t* client, uint32_t window_id, int32_t x, int32_t y)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    payload.x = x;
    payload.y = y;
    return ws_client_request(client, WS_IPC_OP_MOVE_WINDOW, &payload, NULL);
}

int ws_client_raise_window(ws_client_t* client, uint32_t window_id)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    return ws_client_request(client, WS_IPC_OP_RAISE_WINDOW, &payload, NULL);
}

int ws_client_resize_window(ws_client_t* client, uint32_t window_id, uint32_t width, uint32_t height)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    payload.width = width;
    payload.height = height;
    return ws_client_request(client, WS_IPC_OP_RESIZE_WINDOW, &payload, NULL);
}

int ws_client_set_window_color(ws_client_t* client, uint32_t window_id, uint32_t color)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    payload.color = color;
    return ws_client_request(client, WS_IPC_OP_SET_WINDOW_COLOR, &payload, NULL);
}

int ws_client_set_window_visible(ws_client_t* client, uint32_t window_id, bool visible)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    payload.visible = visible ? 1U : 0U;
    return ws_client_request(client, WS_IPC_OP_SET_WINDOW_VISIBLE, &payload, NULL);
}

int ws_client_set_window_title(ws_client_t* client, uint32_t window_id, const char* title)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    ws_copy_string_limit(payload.title, sizeof(payload.title), title);
    return ws_client_request(client, WS_IPC_OP_SET_WINDOW_TITLE, &payload, NULL);
}

int ws_client_set_window_text(ws_client_t* client, uint32_t window_id, const char* text)
{
    ws_ipc_packet_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.window_id = window_id;
    ws_copy_string_limit(payload.text, sizeof(payload.text), text);
    return ws_client_request(client, WS_IPC_OP_SET_WINDOW_TEXT, &payload, NULL);
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

    if (!client->poll_event_inflight)
    {
        ws_ipc_packet_t request = {
            .magic = WS_IPC_MAGIC,
            .version = WS_IPC_VERSION,
            .opcode = (uint16_t) WS_IPC_OP_POLL_EVENT
        };
        if (ws_send_all_with_retry_budget(client->sock_fd, &request, sizeof(request), 8U) < 0)
        {
            if (errno == EWOULDBLOCK)
                errno = EAGAIN;
            return -1;
        }
        client->poll_event_inflight = true;
    }

    ws_ipc_packet_t response;
    if (ws_recv_all_with_retry_budget(client->sock_fd, &response, sizeof(response), 0U) < 0)
    {
        WS_CLIENT_LOG("[AGENTDBG H34 POLL_RECV_FAIL] errno=%d inflight=%u cached=%u\n",
                      errno,
                      client->poll_event_inflight ? 1U : 0U,
                      client->poll_event_cached ? 1U : 0U);
        if (errno == EWOULDBLOCK)
            errno = EAGAIN;
        return -1;
    }
    client->poll_event_inflight = false;

    if (response.magic != WS_IPC_MAGIC || response.version != WS_IPC_VERSION)
    {
        WS_CLIENT_LOG("[AGENTDBG H34 POLL_BAD_HEADER] magic=0x%08X version=%u opcode=%u status=%d\n",
                      (unsigned int) response.magic,
                      (unsigned int) response.version,
                      (unsigned int) response.opcode,
                      response.status);
        errno = EIO;
        return -1;
    }
    if (response.opcode != (uint16_t) WS_IPC_OP_POLL_EVENT)
    {
        WS_CLIENT_LOG("[AGENTDBG H33 IPC_POLL_REORDER] got=%u status=%d inflight=%u cached=%u\n",
                      (unsigned int) response.opcode,
                      response.status,
                      client->poll_event_inflight ? 1U : 0U,
                      client->poll_event_cached ? 1U : 0U);
        /*
         * A delayed response for another opcode can still arrive here in practice.
         * Treat it as a transient reorder instead of a fatal client error to keep
         * the GUI loop alive and let the next iterations resynchronize.
         */
        errno = EAGAIN;
        return -1;
    }

    if (response.status != 0)
    {
        WS_CLIENT_LOG("[AGENTDBG H34 POLL_STATUS_FAIL] status=%d opcode=%u event=%u win=%u\n",
                      response.status,
                      (unsigned int) response.opcode,
                      (unsigned int) response.event_type,
                      (unsigned int) response.window_id);
        errno = (response.status > 0) ? response.status : EIO;
        return -1;
    }

    out_event->type = (ws_event_type_t) response.event_type;
    out_event->window_id = response.window_id;
    out_event->key = response.event_key;
    return 0;
}
