#ifndef WS_TYPES_H
#define WS_TYPES_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <window.h>

#include "ws_config.h"

typedef enum ws_dock_item
{
    WS_DOCK_ITEM_SHELL = 0,
    WS_DOCK_ITEM_SYSTEM_MONITOR = 1,
    WS_DOCK_ITEM_POWER = 2
} ws_dock_item_t;

typedef enum ws_power_item
{
    WS_POWER_ITEM_SHUTDOWN = 0,
    WS_POWER_ITEM_RESTART = 1,
    WS_POWER_ITEM_CANCEL = 2
} ws_power_item_t;

typedef struct ws_client_conn
{
    bool active;
    int fd;
    uint32_t pid;
    ws_client_role_t role;
    uint32_t window_ids[WS_MAX_WINDOWS];
    uint32_t window_count;
    ws_event_t events[WS_CLIENT_EVENT_QUEUE];
    uint32_t event_head;
    uint32_t event_count;
    uint8_t rx_buf[sizeof(ws_ipc_header_t) + WS_IPC_MAX_PAYLOAD_LEN];
    size_t rx_used;
    size_t rx_need;
    uint32_t recv_soft_error_streak;
    bool rx_header_parsed;
    ws_ipc_header_t rx_header;
} ws_client_conn_t;

typedef struct ws_desktop
{
    ws_context_t ws;
    uint32_t frame_index;

    uint32_t top_bar_id;
    uint32_t dock_bar_id;
    uint32_t dock_icon_ids[WS_DOCK_ICON_COUNT];

    bool power_menu_open;
    ws_power_item_t power_focus;
    uint32_t power_dimmer_id;
    uint32_t power_panel_id;
    uint32_t power_button_ids[WS_POWER_ITEM_COUNT];

    bool dragging;
    uint32_t drag_window_id;
    int32_t drag_offset_x;
    int32_t drag_offset_y;

    uint32_t focused_window_id;
    uint32_t focused_slot;

    int listen_fd;
    uint64_t stat_accepts;
    uint64_t stat_accept_errors;
    uint64_t stat_disconnects;
    uint64_t stat_requests;
    uint64_t stat_req_create_ok;
    uint64_t stat_req_create_fail;
    uint64_t stat_recv_fail;
    uint64_t stat_send_fail;
    uint64_t stat_poll_event;
    uint64_t stat_poll_eagain;
    uint64_t stat_set_text;
    uint64_t stat_set_text_noop;
    uint64_t stat_renders;
    uint64_t stat_partial_renders;
    uint64_t stat_render_fail;
    uint64_t stat_render_ticks_total;
    uint64_t stat_render_ticks_max;
    uint64_t stat_render_slow;
    uint64_t stat_render_throttle_skip;
    uint64_t stat_mouse_events;
    uint64_t stat_left_presses;
    pid_t shell_gui_spawn_pid;
    uint64_t shell_gui_spawn_tick;
    pid_t monitor_gui_spawn_pid;
    uint64_t monitor_gui_spawn_tick;
    bool dirty_region_valid;
    int32_t dirty_x0;
    int32_t dirty_y0;
    int32_t dirty_x1;
    int32_t dirty_y1;
    bool compositor_need_full;
    pthread_t render_thread;
    pthread_mutex_t render_mutex;
    pthread_mutex_t ws_mutex;
    bool render_thread_running;
    bool render_thread_exit;
    bool render_full_pending;
    uint32_t tiles_w;
    uint32_t tiles_h;
    uint32_t dirty_tile_count;
    uint8_t* dirty_tiles;
    bool colors_dirty;
    ws_context_t* render_ctx_snapshot;
    ws_client_conn_t clients[WS_CLIENT_MAX];
} ws_desktop_t;

typedef struct ws_loop_profile
{
    uint64_t loops;
    uint64_t loops_activity;
    uint64_t loop_ticks_total;
    uint64_t loop_ticks_max;
    uint64_t mouse_pre_calls;
    uint64_t mouse_pre_ticks_total;
    uint64_t mouse_pre_ticks_max;
    uint64_t mouse_pre_slow;
    uint64_t accept_calls;
    uint64_t accept_ticks_total;
    uint64_t accept_ticks_max;
    uint64_t accept_slow;
    uint64_t poll_calls;
    uint64_t poll_ticks_total;
    uint64_t poll_ticks_max;
    uint64_t poll_slow;
    uint64_t mouse_post_calls;
    uint64_t mouse_post_ticks_total;
    uint64_t mouse_post_ticks_max;
    uint64_t mouse_post_slow;
    uint64_t render_full_count;
    uint64_t render_full_ticks_total;
    uint64_t render_full_ticks_max;
    uint64_t render_region_count;
    uint64_t render_region_ticks_total;
    uint64_t render_region_ticks_max;
    uint64_t render_region_pixels_total;
    uint64_t render_cursor_count;
    uint64_t render_cursor_ticks_total;
    uint64_t render_cursor_ticks_max;
} ws_loop_profile_t;

#endif
