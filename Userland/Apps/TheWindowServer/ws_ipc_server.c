#include "Includes/ws_ipc_server.h"
#include "Includes/ws_compositor.h"
#include "Includes/ws_focus.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/filio.h>

/* ------------------------------------------------------------------ */
/*  Static helpers                                                     */
/* ------------------------------------------------------------------ */

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

static ws_client_role_t ws_role_from_u32(uint32_t raw)
{
    if (raw == (uint32_t) WS_CLIENT_ROLE_SHELL_GUI)
        return WS_CLIENT_ROLE_SHELL_GUI;
    if (raw == (uint32_t) WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI)
        return WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI;
    return WS_CLIENT_ROLE_GENERIC;
}

static size_t ws_window_body_strlen_bounded(const char* s, size_t cap)
{
    if (!s || cap == 0U)
        return 0U;

    for (size_t i = 0U; i < cap; i++)
    {
        if (s[i] == '\0')
            return i;
    }
    return cap;
}

static bool ws_window_body_text_equal(const char* a, const char* b)
{
    if (!a || !b)
        return false;

    const size_t cap = (size_t) WS_WINDOW_BODY_TEXT_MAX + 1U;
    size_t la = ws_window_body_strlen_bounded(a, cap);
    size_t lb = ws_window_body_strlen_bounded(b, cap);
    if (la != lb)
        return false;
    return memcmp(a, b, la) == 0;
}

/* ------------------------------------------------------------------ */
/*  Client slot management                                             */
/* ------------------------------------------------------------------ */

void ws_client_reset_slot(ws_client_conn_t* client)
{
    if (!client)
        return;

    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->role = WS_CLIENT_ROLE_GENERIC;
}

bool ws_client_add_window(ws_client_conn_t* client, uint32_t window_id)
{
    if (!client || window_id == 0U)
        return false;
    if (client->window_count >= WS_MAX_WINDOWS)
        return false;

    client->window_ids[client->window_count++] = window_id;
    return true;
}

void ws_client_remove_window(ws_client_conn_t* client, uint32_t window_id)
{
    if (!client || window_id == 0U)
        return;

    for (uint32_t i = 0U; i < client->window_count; i++)
    {
        if (client->window_ids[i] != window_id)
            continue;

        for (uint32_t j = i; j + 1U < client->window_count; j++)
            client->window_ids[j] = client->window_ids[j + 1U];
        if (client->window_count > 0U)
            client->window_count--;
        return;
    }
}

bool ws_client_owns_window(const ws_client_conn_t* client, uint32_t window_id)
{
    if (!client || window_id == 0U)
        return false;

    for (uint32_t i = 0U; i < client->window_count; i++)
    {
        if (client->window_ids[i] == window_id)
            return true;
    }

    return false;
}

bool ws_find_client_by_window(const ws_desktop_t* desktop, uint32_t window_id, uint32_t* out_slot)
{
    if (!desktop || window_id == 0U)
        return false;

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        const ws_client_conn_t* client = &desktop->clients[i];
        if (!client->active)
            continue;
        if (!ws_client_owns_window(client, window_id))
            continue;

        if (out_slot)
            *out_slot = i;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Event ring buffer                                                  */
/* ------------------------------------------------------------------ */

bool ws_client_event_push(ws_client_conn_t* client, const ws_event_t* event)
{
    if (!client || !event)
        return false;

    uint32_t write_index = 0U;
    if (client->event_count < WS_CLIENT_EVENT_QUEUE)
    {
        write_index = (client->event_head + client->event_count) % WS_CLIENT_EVENT_QUEUE;
        client->event_count++;
    }
    else
    {
        write_index = client->event_head;
        client->event_head = (client->event_head + 1U) % WS_CLIENT_EVENT_QUEUE;
    }

    client->events[write_index] = *event;
    return true;
}

bool ws_client_event_pop(ws_client_conn_t* client, ws_event_t* out_event)
{
    if (!client || !out_event || client->event_count == 0U)
        return false;

    *out_event = client->events[client->event_head];
    client->event_head = (client->event_head + 1U) % WS_CLIENT_EVENT_QUEUE;
    client->event_count--;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Window cleanup & disconnect                                        */
/* ------------------------------------------------------------------ */

void ws_client_destroy_all_windows(ws_desktop_t* desktop, ws_client_conn_t* client)
{
    if (!desktop || !client)
        return;

    while (client->window_count > 0U)
    {
        uint32_t window_id = client->window_ids[client->window_count - 1U];
        ws_desktop_invalidate_window_id(desktop, window_id);
        ws_ctx_lock(desktop);
        (void) ws_destroy_window(&desktop->ws, window_id);
        ws_ctx_unlock(desktop);
        client->window_count--;
    }
}

void ws_client_disconnect_slot(ws_desktop_t* desktop, uint32_t slot)
{
    if (!desktop || slot >= WS_CLIENT_MAX)
        return;

    ws_client_conn_t* client = &desktop->clients[slot];
    if (!client->active)
        return;

    WS_LOG("disconnect slot=%u fd=%d pid=%u role=%u windows=%u\n",
           (unsigned int) slot,
           client->fd,
           (unsigned int) client->pid,
           (unsigned int) client->role,
           (unsigned int) client->window_count);
    desktop->stat_disconnects++;

    ws_client_destroy_all_windows(desktop, client);
    if (client->fd >= 0)
        (void) close(client->fd);
    ws_client_reset_slot(client);
    desktop->colors_dirty = true;
}

/* ------------------------------------------------------------------ */
/*  Server socket                                                      */
/* ------------------------------------------------------------------ */

int ws_server_socket_open(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        WS_LOG("server socket() failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(WS_SERVER_SOCK_PATH);
    if (path_len >= sizeof(addr.sun_path))
        path_len = sizeof(addr.sun_path) - 1U;
    memcpy(addr.sun_path, WS_SERVER_SOCK_PATH, path_len);
    addr.sun_path[path_len] = '\0';

    if (bind(fd, (const struct sockaddr*) &addr, (socklen_t)(sizeof(sa_family_t) + path_len + 1U)) < 0)
    {
        int saved_errno = errno;
        WS_LOG("server bind() failed errno=%d\n", saved_errno);
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    if (listen(fd, WS_SERVER_BACKLOG) < 0)
    {
        int saved_errno = errno;
        WS_LOG("server listen() failed errno=%d\n", saved_errno);
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    if (ws_set_socket_non_blocking(fd) < 0)
    {
        int saved_errno = errno;
        WS_LOG("server FIONBIO failed errno=%d\n", saved_errno);
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    WS_LOG("server listening on %s fd=%d\n", WS_SERVER_SOCK_PATH, fd);
    return fd;
}

/* ------------------------------------------------------------------ */
/*  IPC send helpers                                                   */
/* ------------------------------------------------------------------ */

static int ws_ipc_send_bytes(int fd, const void* data, size_t len)
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

        ssize_t rc = send(fd, src + sent, chunk, MSG_DONTWAIT);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (retry_budget >= WS_IPC_SEND_RETRY_BUDGET)
                    return -1;
                retry_budget++;
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
        retry_budget = 0U;
        sent += (size_t) rc;
    }

    return 0;
}

int ws_ipc_send_response(int fd, uint16_t opcode, int32_t status,
                         uint32_t window_id, const void* payload, uint32_t payload_len)
{
    ws_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WS_IPC_MAGIC;
    hdr.version = WS_IPC_VERSION;
    hdr.opcode = opcode;
    hdr.status = status;
    hdr.window_id = window_id;
    hdr.payload_len = payload_len;

    if (ws_ipc_send_bytes(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    if (payload && payload_len > 0U)
    {
        if (ws_ipc_send_bytes(fd, payload, payload_len) < 0)
            return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Request dispatch                                                   */
/* ------------------------------------------------------------------ */

static void ws_handle_client_message(ws_desktop_t* desktop,
                                     uint32_t slot,
                                     const ws_ipc_header_t* header,
                                     const uint8_t* payload,
                                     bool* out_changed)
{
    if (!desktop || slot >= WS_CLIENT_MAX || !header)
        return;

    ws_client_conn_t* client = &desktop->clients[slot];
    desktop->stat_requests++;
    if (out_changed)
        *out_changed = false;

    uint16_t opcode = header->opcode;
    uint32_t window_id = header->window_id;
    uint32_t plen = header->payload_len;

    if (WS_IPC_TRACE)
    {
        WS_LOG("request slot=%u pid=%u opcode=%u win=%u plen=%u\n",
               (unsigned int) slot,
               (unsigned int) client->pid,
               (unsigned int) opcode,
               (unsigned int) window_id,
               (unsigned int) plen);
    }

    switch ((ws_ipc_opcode_t) opcode)
    {
        case WS_IPC_OP_HELLO:
        {
            if (plen < sizeof(ws_ipc_hello_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, 0U, NULL, 0U);
                return;
            }

            ws_ipc_hello_payload_t hello;
            memcpy(&hello, payload, sizeof(hello));
            client->pid = hello.owner_pid;
            client->role = ws_role_from_u32(hello.client_role);
            WS_LOG("hello slot=%u fd=%d pid=%u role=%u\n",
                   (unsigned int) slot,
                   client->fd,
                   (unsigned int) client->pid,
                   (unsigned int) client->role);
            (void) ws_ipc_send_response(client->fd, opcode, 0, 0U, NULL, 0U);
            return;
        }

        case WS_IPC_OP_CREATE_WINDOW:
        {
            if (plen < sizeof(ws_ipc_create_window_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, 0U, NULL, 0U);
                return;
            }

            ws_ipc_create_window_payload_t cw;
            memcpy(&cw, payload, sizeof(cw));

            char safe_title[WS_WINDOW_TITLE_MAX + 1U];
            safe_title[0] = '\0';
            if (plen > sizeof(cw))
            {
                const char* title_src = (const char*) (payload + sizeof(cw));
                size_t title_avail = plen - sizeof(cw);
                ws_copy_cstr_bounded(safe_title, sizeof(safe_title), title_src);
                (void) title_avail;
            }

            ws_window_desc_t desc;
            memset(&desc, 0, sizeof(desc));
            desc.x = cw.x;
            desc.y = cw.y;
            desc.width = cw.width;
            desc.height = cw.height;
            desc.color = cw.color;
            desc.border_color = cw.border_color;
            desc.titlebar_color = cw.titlebar_color;
            desc.visible = cw.visible != 0U;
            desc.frame_controls = cw.frame_controls != 0U;
            desc.title = safe_title;

            WS_LOG("create-window req slot=%u pid=%u x=%d y=%d w=%u h=%u title='%s'\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   desc.x,
                   desc.y,
                   (unsigned int) desc.width,
                   (unsigned int) desc.height,
                   safe_title);

            uint32_t new_window_id = 0U;
            ws_ctx_lock(desktop);
            int rc_create = ws_create_window(&desktop->ws, &desc, &new_window_id);
            ws_ctx_unlock(desktop);
            if (rc_create < 0)
            {
                desktop->stat_req_create_fail++;
                WS_LOG("create-window failed slot=%u pid=%u errno=%d\n",
                       (unsigned int) slot,
                       (unsigned int) client->pid,
                       errno);
                (void) ws_ipc_send_response(client->fd, opcode, errno, 0U, NULL, 0U);
                return;
            }

            if (!ws_client_add_window(client, new_window_id))
            {
                ws_ctx_lock(desktop);
                (void) ws_destroy_window(&desktop->ws, new_window_id);
                ws_ctx_unlock(desktop);
                desktop->stat_req_create_fail++;
                WS_LOG("create-window add-owner failed slot=%u pid=%u win=%u\n",
                       (unsigned int) slot,
                       (unsigned int) client->pid,
                       (unsigned int) new_window_id);
                (void) ws_ipc_send_response(client->fd, opcode, ENOSPC, 0U, NULL, 0U);
                return;
            }

            ws_ctx_lock(desktop);
            (void) ws_raise_window(&desktop->ws, new_window_id);
            ws_ctx_unlock(desktop);
            ws_set_focus(desktop, new_window_id, slot);
            desktop->compositor_need_full = true;
            desktop->stat_req_create_ok++;
            WS_LOG("create-window ok slot=%u pid=%u win=%u title='%s'\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   (unsigned int) new_window_id,
                   safe_title);
            ws_desktop_invalidate_window_id(desktop, new_window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, new_window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_DESTROY_WINDOW:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_destroy = ws_destroy_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            if (rc_destroy < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_client_remove_window(client, window_id);
            WS_LOG("destroy-window slot=%u pid=%u win=%u\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   (unsigned int) window_id);
            if (desktop->focused_window_id == window_id)
                ws_refresh_focus_after_close(desktop);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_MOVE_WINDOW:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen < sizeof(ws_ipc_move_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            ws_ipc_move_payload_t mv;
            memcpy(&mv, payload, sizeof(mv));

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_move = ws_move_window(&desktop->ws, window_id, mv.x, mv.y);
            ws_ctx_unlock(desktop);
            if (rc_move < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_RAISE_WINDOW:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }

            uint32_t top_id = 0U;
            if (desktop->ws.window_count > 0U)
                top_id = desktop->ws.windows[desktop->ws.window_count - 1U].id;
            if (window_id == top_id)
            {
                (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
                return;
            }

            ws_ctx_lock(desktop);
            int rc_raise = ws_raise_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            if (rc_raise < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            desktop->compositor_need_full = true;
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_RESIZE_WINDOW:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen < sizeof(ws_ipc_resize_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            ws_ipc_resize_payload_t rs;
            memcpy(&rs, payload, sizeof(rs));

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_resize = ws_resize_window(&desktop->ws, window_id, rs.width, rs.height);
            ws_ctx_unlock(desktop);
            if (rc_resize < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_COLOR:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen < sizeof(ws_ipc_color_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            ws_ipc_color_payload_t cp;
            memcpy(&cp, payload, sizeof(cp));

            ws_ctx_lock(desktop);
            int rc_color = ws_set_window_color(&desktop->ws, window_id, cp.color);
            ws_ctx_unlock(desktop);
            if (rc_color < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_VISIBLE:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen < sizeof(ws_ipc_visible_payload_t) || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            ws_ipc_visible_payload_t vp;
            memcpy(&vp, payload, sizeof(vp));

            ws_ctx_lock(desktop);
            int rc_visible = ws_set_window_visible(&desktop->ws, window_id, vp.visible != 0U);
            ws_ctx_unlock(desktop);
            if (rc_visible < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_TITLE:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen == 0U || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            char safe_title[WS_WINDOW_TITLE_MAX + 1U];
            ws_copy_cstr_bounded(safe_title, sizeof(safe_title), (const char*) payload);

            ws_ctx_lock(desktop);
            int rc_title = ws_set_window_title(&desktop->ws, window_id, safe_title);
            ws_ctx_unlock(desktop);
            if (rc_title < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_TEXT:
        {
            if (!ws_client_owns_window(client, window_id))
            {
                (void) ws_ipc_send_response(client->fd, opcode, EPERM, window_id, NULL, 0U);
                return;
            }
            if (plen == 0U || !payload)
            {
                (void) ws_ipc_send_response(client->fd, opcode, EINVAL, window_id, NULL, 0U);
                return;
            }

            char safe_text[WS_WINDOW_BODY_TEXT_MAX + 1U];
            ws_copy_cstr_bounded(safe_text, sizeof(safe_text), (const char*) payload);

            ws_window_t window_before;
            ws_ctx_lock(desktop);
            bool have_window = ws_find_window(&desktop->ws, window_id, &window_before) == 0;
            ws_ctx_unlock(desktop);
            if (have_window && ws_window_body_text_equal(window_before.body_text, safe_text))
            {
                desktop->stat_set_text_noop++;
                (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
                return;
            }

            desktop->stat_set_text++;
            ws_ctx_lock(desktop);
            int rc_text = ws_set_window_text(&desktop->ws, window_id, safe_text);
            ws_ctx_unlock(desktop);
            if (rc_text < 0)
            {
                (void) ws_ipc_send_response(client->fd, opcode, errno, window_id, NULL, 0U);
                return;
            }

            if (!have_window || !ws_invalidate_window_text_delta(desktop,
                                                                 &window_before,
                                                                 window_before.body_text,
                                                                 safe_text))
            {
                ws_desktop_invalidate_window_id(desktop, window_id);
            }
            (void) ws_ipc_send_response(client->fd, opcode, 0, window_id, NULL, 0U);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_POLL_EVENT:
        {
            desktop->stat_poll_event++;
            ws_event_t event;
            if (!ws_client_event_pop(client, &event))
            {
                desktop->stat_poll_eagain++;
                (void) ws_ipc_send_response(client->fd, opcode, EAGAIN, 0U, NULL, 0U);
                return;
            }

            ws_ipc_event_payload_t ep;
            ep.event_type = (uint32_t) event.type;
            ep.event_key = event.key;
            (void) ws_ipc_send_response(client->fd, opcode, 0, event.window_id,
                                        &ep, (uint32_t) sizeof(ep));
            return;
        }

        default:
            WS_LOG("unknown opcode slot=%u fd=%d opcode=%u win=%u plen=%u magic=0x%08X ver=%u\n",
                   (unsigned int) slot,
                   client->fd,
                   (unsigned int) opcode,
                   (unsigned int) window_id,
                   (unsigned int) plen,
                   (unsigned int) header->magic,
                   (unsigned int) header->version);
            (void) ws_ipc_send_response(client->fd, opcode, ENOTSUP, window_id, NULL, 0U);
            return;
    }
}

/* ------------------------------------------------------------------ */
/*  Accept loop                                                        */
/* ------------------------------------------------------------------ */

bool ws_accept_clients(ws_desktop_t* desktop)
{
    if (!desktop || desktop->listen_fd < 0)
        return false;

    bool changed = false;
    uint32_t accepted_count = 0U;
    while (accepted_count < WS_ACCEPT_BUDGET_PER_TICK)
    {
        int fd = accept(desktop->listen_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            desktop->stat_accept_errors++;
            WS_LOG("accept failed errno=%d\n", errno);
            break;
        }

        uint32_t free_slot = WS_CLIENT_MAX;
        for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        {
            if (!desktop->clients[i].active)
            {
                free_slot = i;
                break;
            }
        }

        if (free_slot >= WS_CLIENT_MAX)
        {
            WS_LOG("accept refused (no free slot) fd=%d\n", fd);
            (void) close(fd);
            continue;
        }

        if (ws_set_socket_non_blocking(fd) < 0)
        {
            desktop->stat_accept_errors++;
            WS_LOG("accept set-nonblock failed fd=%d errno=%d\n", fd, errno);
            (void) close(fd);
            continue;
        }

        ws_client_conn_t* client = &desktop->clients[free_slot];
        ws_client_reset_slot(client);
        client->active = true;
        client->fd = fd;
        desktop->stat_accepts++;
        desktop->colors_dirty = true;
        WS_LOG("accept slot=%u fd=%d active_clients=%u\n",
               (unsigned int) free_slot,
               fd,
               (unsigned int) ws_active_client_count(desktop));
        accepted_count++;
        changed = true;
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/*  Poll / recv state machine                                          */
/* ------------------------------------------------------------------ */

bool ws_poll_clients(ws_desktop_t* desktop, bool* out_activity)
{
    if (!desktop)
        return false;

    bool changed = false;
    bool activity = false;

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        ws_client_conn_t* client = &desktop->clients[i];
        if (!client->active || client->fd < 0)
            continue;

        bool keep_running = true;
        uint32_t processed_requests = 0U;
        uint32_t io_iterations = 0U;
        while (keep_running &&
               processed_requests < WS_CLIENT_REQ_BUDGET_PER_TICK &&
               io_iterations < WS_CLIENT_IO_ITER_BUDGET)
        {
            io_iterations++;

            size_t want;
            if (!client->rx_header_parsed)
                want = sizeof(ws_ipc_header_t) - client->rx_used;
            else
                want = client->rx_need - client->rx_used;

            if (want == 0U)
                want = 1U;
            if (want > sizeof(client->rx_buf) - client->rx_used)
                want = sizeof(client->rx_buf) - client->rx_used;

            ssize_t rc = recv(client->fd,
                              client->rx_buf + client->rx_used,
                              want,
                              MSG_DONTWAIT);
            if (rc < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    client->recv_soft_error_streak = 0U;
                    break;
                }
                if (errno == EINTR)
                    continue;

                if ((errno == ECONNRESET || errno == ENOTCONN) &&
                    client->recv_soft_error_streak < WS_CLIENT_RECV_SOFT_ERR_BUDGET)
                {
                    client->recv_soft_error_streak++;
                    if (client->recv_soft_error_streak == 1U ||
                        client->recv_soft_error_streak == WS_CLIENT_RECV_SOFT_ERR_BUDGET)
                    {
                        WS_LOG("recv soft-fail slot=%u fd=%d errno=%d streak=%u/%u\n",
                               (unsigned int) i,
                               client->fd,
                               errno,
                               (unsigned int) client->recv_soft_error_streak,
                               (unsigned int) WS_CLIENT_RECV_SOFT_ERR_BUDGET);
                    }
                    (void) sched_yield();
                    break;
                }

                client->recv_soft_error_streak = 0U;
                desktop->stat_recv_fail++;
                WS_LOG("recv failed slot=%u fd=%d errno=%d\n",
                       (unsigned int) i,
                       client->fd,
                       errno);
                ws_client_disconnect_slot(desktop, i);
                changed = true;
                break;
            }

            if (rc == 0)
            {
                client->recv_soft_error_streak = 0U;
                WS_LOG("recv EOF slot=%u fd=%d\n", (unsigned int) i, client->fd);
                ws_client_disconnect_slot(desktop, i);
                changed = true;
                break;
            }

            client->recv_soft_error_streak = 0U;
            client->rx_used += (size_t) rc;

            if (!client->rx_header_parsed)
            {
                if (client->rx_used < sizeof(ws_ipc_header_t))
                    continue;

                memcpy(&client->rx_header, client->rx_buf, sizeof(ws_ipc_header_t));

                if (client->rx_header.magic != WS_IPC_MAGIC ||
                    client->rx_header.version != WS_IPC_VERSION)
                {
                    WS_LOG("bad header slot=%u fd=%d magic=0x%08X version=%u\n",
                           (unsigned int) i,
                           client->fd,
                           (unsigned int) client->rx_header.magic,
                           (unsigned int) client->rx_header.version);
                    ws_client_disconnect_slot(desktop, i);
                    changed = true;
                    break;
                }

                if (client->rx_header.payload_len > WS_IPC_MAX_PAYLOAD_LEN)
                {
                    WS_LOG("payload too large slot=%u fd=%d plen=%u max=%u\n",
                           (unsigned int) i,
                           client->fd,
                           (unsigned int) client->rx_header.payload_len,
                           (unsigned int) WS_IPC_MAX_PAYLOAD_LEN);
                    ws_client_disconnect_slot(desktop, i);
                    changed = true;
                    break;
                }

                client->rx_need = sizeof(ws_ipc_header_t) + client->rx_header.payload_len;
                client->rx_header_parsed = true;
            }

            if (client->rx_used < client->rx_need)
                continue;

            const uint8_t* payload_ptr = NULL;
            if (client->rx_header.payload_len > 0U)
                payload_ptr = client->rx_buf + sizeof(ws_ipc_header_t);

            bool request_changed = false;
            ws_handle_client_message(desktop, i, &client->rx_header, payload_ptr, &request_changed);
            processed_requests++;

            if (request_changed)
                changed = true;
            if (request_changed ||
                client->rx_header.opcode != (uint16_t) WS_IPC_OP_POLL_EVENT)
                activity = true;

            /* Check if the send path disconnected this client. */
            if (!client->active)
            {
                keep_running = false;
                break;
            }

            /* Reset rx state for next message. */
            client->rx_used = 0U;
            client->rx_need = 0U;
            client->rx_header_parsed = false;
            memset(&client->rx_header, 0, sizeof(client->rx_header));
        }
    }

    if (out_activity)
        *out_activity = activity;
    return changed;
}

/* ------------------------------------------------------------------ */
/*  Query helpers                                                      */
/* ------------------------------------------------------------------ */

uint32_t ws_active_client_count(const ws_desktop_t* desktop)
{
    if (!desktop)
        return 0U;

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        if (desktop->clients[i].active)
            count++;
    }

    return count;
}

uint32_t ws_active_client_windows(const ws_desktop_t* desktop)
{
    if (!desktop)
        return 0U;

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        count += desktop->clients[i].window_count;

    return count;
}

bool ws_role_has_live_client(const ws_desktop_t* desktop, ws_client_role_t role)
{
    if (!desktop)
        return false;

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        const ws_client_conn_t* client = &desktop->clients[i];
        if (!client->active)
            continue;
        if (client->role != role)
            continue;
        if (client->window_count == 0U)
            continue;

        return true;
    }

    return false;
}

bool ws_role_ipc_session_open(const ws_desktop_t* desktop, ws_client_role_t role)
{
    if (!desktop)
        return false;

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        const ws_client_conn_t* client = &desktop->clients[i];
        if (!client->active)
            continue;
        if (client->role != role)
            continue;
        return true;
    }

    return false;
}
