#ifndef _WINDOW_H
#define _WINDOW_H

#include <UAPI/DRM.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_MAX_WINDOWS 32U
#define WS_WINDOW_TITLE_MAX 63U
#define WS_WINDOW_BODY_TEXT_MAX 2047U
#define WS_SERVER_PORT 6090U
#define WS_IPC_MAGIC 0x57534950U
#define WS_IPC_VERSION 1U
#define WS_CURSOR_WIDTH 12U
#define WS_CURSOR_HEIGHT 16U
#define WS_CURSOR_SHADOW_OFFSET 1U

typedef struct ws_window_desc
{
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t border_color;
    uint32_t titlebar_color;
    bool visible;
    bool frame_controls;
    const char* title;
} ws_window_desc_t;

typedef enum ws_client_role
{
    WS_CLIENT_ROLE_GENERIC = 0,
    WS_CLIENT_ROLE_SHELL_GUI = 1,
    WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI = 2
} ws_client_role_t;

typedef enum ws_event_type
{
    WS_EVENT_NONE = 0,
    WS_EVENT_CLOSE = 1,
    WS_EVENT_KEY = 2
} ws_event_type_t;

typedef struct ws_event
{
    ws_event_type_t type;
    uint32_t window_id;
    int32_t key;
} ws_event_t;

typedef enum ws_ipc_opcode
{
    WS_IPC_OP_HELLO = 1,
    WS_IPC_OP_CREATE_WINDOW = 2,
    WS_IPC_OP_DESTROY_WINDOW = 3,
    WS_IPC_OP_MOVE_WINDOW = 4,
    WS_IPC_OP_RAISE_WINDOW = 5,
    WS_IPC_OP_RESIZE_WINDOW = 6,
    WS_IPC_OP_SET_WINDOW_COLOR = 7,
    WS_IPC_OP_SET_WINDOW_VISIBLE = 8,
    WS_IPC_OP_SET_WINDOW_TITLE = 9,
    WS_IPC_OP_SET_WINDOW_TEXT = 10,
    WS_IPC_OP_POLL_EVENT = 11
} ws_ipc_opcode_t;

typedef struct ws_ipc_packet
{
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    int32_t status;
    uint32_t window_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t border_color;
    uint32_t titlebar_color;
    uint32_t visible;
    uint32_t frame_controls;
    uint32_t owner_pid;
    uint32_t client_role;
    uint32_t event_type;
    int32_t event_key;
    char title[WS_WINDOW_TITLE_MAX + 1U];
    char text[WS_WINDOW_BODY_TEXT_MAX + 1U];
} ws_ipc_packet_t;

typedef struct ws_client
{
    int sock_fd;
    uint32_t owner_pid;
    ws_client_role_t role;
    bool connected;
    bool poll_event_inflight;
    bool poll_event_cached;
    ws_event_t cached_event;
} ws_client_t;

typedef struct ws_window
{
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t border_color;
    uint32_t titlebar_color;
    bool visible;
    bool frame_controls;
    char title[WS_WINDOW_TITLE_MAX + 1U];
    char body_text[WS_WINDOW_BODY_TEXT_MAX + 1U];
} ws_window_t;

typedef struct ws_context
{
    int drm_fd;
    int dmabuf_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t dumb_handle;
    uint32_t dumb_pitch;
    uint64_t dumb_size;
    uint8_t* frame_map;
    uint32_t mode_blob_id;
    drm_mode_modeinfo_t mode;
    drm_mode_atomic_req_t atomic_req;
    bool ready;
    bool master_owned;
    uint32_t desktop_color;
    bool font_ready;
    uint8_t* font_bitmap;
    size_t font_bitmap_size;
    uint32_t font_width;
    uint32_t font_height;
    uint32_t font_num_glyph;
    uint32_t font_bytes_per_glyph;
    uint32_t font_row_bytes;
    bool cursor_visible;
    int32_t cursor_x;
    int32_t cursor_y;
    uint32_t cursor_color;
    bool clip_enabled;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
    uint32_t next_window_id;
    uint32_t window_count;
    ws_window_t windows[WS_MAX_WINDOWS];
} ws_context_t;

int ws_open(ws_context_t* ctx, bool take_master);
void ws_close(ws_context_t* ctx);

int ws_set_desktop_color(ws_context_t* ctx, uint32_t color);
int ws_create_window(ws_context_t* ctx, const ws_window_desc_t* desc, uint32_t* out_window_id);
int ws_destroy_window(ws_context_t* ctx, uint32_t window_id);
int ws_move_window(ws_context_t* ctx, uint32_t window_id, int32_t x, int32_t y);
int ws_raise_window(ws_context_t* ctx, uint32_t window_id);
int ws_resize_window(ws_context_t* ctx, uint32_t window_id, uint32_t width, uint32_t height);
int ws_set_window_color(ws_context_t* ctx, uint32_t window_id, uint32_t color);
int ws_set_window_visible(ws_context_t* ctx, uint32_t window_id, bool visible);
int ws_set_window_title(ws_context_t* ctx, uint32_t window_id, const char* title);
int ws_set_window_text(ws_context_t* ctx, uint32_t window_id, const char* text);
int ws_find_window(const ws_context_t* ctx, uint32_t window_id, ws_window_t* out_window);
int ws_set_cursor_visible(ws_context_t* ctx, bool visible);
int ws_set_cursor_position(ws_context_t* ctx, int32_t x, int32_t y);
int ws_set_cursor_color(ws_context_t* ctx, uint32_t color);
int ws_render(ws_context_t* ctx);
int ws_render_region(ws_context_t* ctx, int32_t x, int32_t y, uint32_t width, uint32_t height);

int ws_client_connect(ws_client_t* client, ws_client_role_t role);
void ws_client_disconnect(ws_client_t* client);
int ws_client_create_window(ws_client_t* client, const ws_window_desc_t* desc, uint32_t* out_window_id);
int ws_client_destroy_window(ws_client_t* client, uint32_t window_id);
int ws_client_move_window(ws_client_t* client, uint32_t window_id, int32_t x, int32_t y);
int ws_client_raise_window(ws_client_t* client, uint32_t window_id);
int ws_client_resize_window(ws_client_t* client, uint32_t window_id, uint32_t width, uint32_t height);
int ws_client_set_window_color(ws_client_t* client, uint32_t window_id, uint32_t color);
int ws_client_set_window_visible(ws_client_t* client, uint32_t window_id, bool visible);
int ws_client_set_window_title(ws_client_t* client, uint32_t window_id, const char* title);
int ws_client_set_window_text(ws_client_t* client, uint32_t window_id, const char* text);
int ws_client_poll_event(ws_client_t* client, ws_event_t* out_event);

#endif
