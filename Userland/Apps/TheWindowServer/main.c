#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <window.h>

#define WS_ACTIVE_SLEEP_US      200U
#define WS_IDLE_SLEEP_US        1000U
#define WS_TITLEBAR_HEIGHT      20U
#define WS_CLOSE_BUTTON_SIZE    14U
#define WS_CLOSE_BUTTON_MARGINX 4U
#define WS_CLOSE_BUTTON_MARGINY 3U
#define WS_WINDOW_BODY_PAD_X    6U
#define WS_WINDOW_BODY_PAD_Y    4U
#define WS_WINDOW_BODY_BOTTOM_MARGIN 3U
#define WS_RENDER_TILE_W 128U
#define WS_RENDER_TILE_H 128U
#define WS_RENDER_THREAD_SLEEP_US 1000U
#define WS_RENDER_TILE_BUDGET 64U
#define WS_RENDER_THREAD_YIELD_US 200U
#define WS_RENDER_TIME_BUDGET_TICKS 2ULL
#define WS_SERVER_BACKLOG       16
#define WS_CLIENT_MAX           16U
#define WS_CLIENT_EVENT_QUEUE   32U
#define WS_IPC_SEND_CHUNK_BYTES 1024U
#define WS_IPC_SEND_RETRY_BUDGET 8U
#define WS_ACCEPT_BUDGET_PER_TICK 16U
#define WS_CLIENT_REQ_BUDGET_PER_TICK 24U
#define WS_CLIENT_IO_ITER_BUDGET 96U
#define WS_MOUSE_EVENT_BUDGET_PER_TICK 256U
#define WS_MOUSE_EVENT_BUDGET_AFTER_CLIENTS 96U
#define WS_KEY_EVENT_BUDGET_PER_TICK 64U
#define WS_MOUSE_ACCEL_SMALL_MULT 1
#define WS_MOUSE_ACCEL_MEDIUM_MULT 1
#define WS_RENDER_MIN_INTERVAL_TICKS 1ULL
/* Au-delà de ce seuil (ticks sys_tick) un rendu compte comme « lent » dans les stats. */
#define WS_RENDER_SLOW_TICKS 32ULL
#define WS_FIRST_RENDER_DELAY_TICKS 25ULL
#define WS_LAUNCHER_PORT 6091U
#define WS_LAUNCHER_MAGIC 0x57534C43U
/* Heartbeat verbeux sur le port série ralentit fortement la boucle (printf bloquant). */
#define WS_HEARTBEAT_TICKS 4000ULL
#ifndef WS_HEARTBEAT_VERBOSE
#define WS_HEARTBEAT_VERBOSE 0
#endif
#ifndef WS_IPC_TRACE
#define WS_IPC_TRACE 0
#endif
#define WS_LAUNCH_PENDING_TIMEOUT_TICKS 300ULL
#define WS_STAGE_SLOW_TICKS     2ULL
#ifndef WS_PROFILE_ENABLED
#define WS_PROFILE_ENABLED 1
#endif
#ifndef WS_PROFILE_LOG_TICKS
#define WS_PROFILE_LOG_TICKS 2000ULL
#endif

#define WS_MOUSE_BUTTON_LEFT 0x01U

#define WS_DOCK_ICON_COUNT  3U
#define WS_POWER_ITEM_COUNT 3U

#define WS_LOG(fmt, ...)                                                                                                    \
    do                                                                                                                      \
    {                                                                                                                       \
        unsigned long long __tick = (unsigned long long) sys_tick_get();                                                   \
        printf("[TheWindowServer t=%llu] " fmt, __tick, ##__VA_ARGS__);                                                   \
    }                                                                                                                       \
    while (0)

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

typedef enum ws_launch_cmd
{
    WS_LAUNCH_CMD_NONE = 0,
    WS_LAUNCH_CMD_SHELL_GUI = 1,
    WS_LAUNCH_CMD_SYSTEM_MONITOR_GUI = 2,
    WS_LAUNCH_CMD_POWER_SHUTDOWN = 3,
    WS_LAUNCH_CMD_POWER_RESTART = 4
} ws_launch_cmd_t;


typedef struct ws_launch_msg
{
    uint32_t magic;
    uint32_t cmd;
} ws_launch_msg_t;

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
    uint8_t rx_buf[sizeof(ws_ipc_packet_t)];
    size_t rx_used;
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
    pid_t launcher_pid;
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
    uint64_t stat_launcher_cmd_sent;
    uint64_t stat_launcher_cmd_send_fail;
    uint64_t stat_launcher_cmd_recv;
    uint64_t stat_launcher_spawn_fail;
    bool shell_launch_pending;
    uint64_t shell_launch_tick;
    bool monitor_launch_pending;
    uint64_t monitor_launch_tick;
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

static ws_desktop_t desktop;

static void ws_set_focus(ws_desktop_t* desktop, uint32_t window_id, uint32_t slot);
static void ws_refresh_focus_after_close(ws_desktop_t* desktop);

static int32_t ws_abs_i32(int32_t value)
{
    return (value < 0) ? -value : value;
}

static int32_t ws_scale_mouse_delta(int16_t raw)
{
    int32_t d = (int32_t) raw;
    int32_t ad = ws_abs_i32(d);

    if (ad == 0)
        return 0;
    if (ad <= 2)
        return d * WS_MOUSE_ACCEL_SMALL_MULT;
    if (ad <= 6)
        return d * WS_MOUSE_ACCEL_MEDIUM_MULT;

    return d;
}

static uint32_t ws_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint64_t ws_tick_delta(uint64_t end_tick, uint64_t start_tick)
{
    return (end_tick >= start_tick) ? (end_tick - start_tick) : 0ULL;
}

static void ws_copy_cstr_bounded(char* out, size_t out_size, const char* in)
{
    if (!out || out_size == 0U)
        return;

    out[0] = '\0';
    if (!in)
        return;

    size_t max_copy = out_size - 1U;
    size_t len = 0U;
    while (len < max_copy && in[len] != '\0')
        len++;
    memcpy(out, in, len);
    out[len] = '\0';
}

static void ws_profile_stage_record(uint64_t* calls,
                                    uint64_t* ticks_total,
                                    uint64_t* ticks_max,
                                    uint64_t* slow_count,
                                    uint64_t ticks)
{
    if (!calls || !ticks_total || !ticks_max || !slow_count)
        return;

    (*calls)++;
    *ticks_total += ticks;
    if (ticks > *ticks_max)
        *ticks_max = ticks;
    if (ticks >= WS_STAGE_SLOW_TICKS)
        (*slow_count)++;
}

static void ws_apply_desktop_colors(ws_desktop_t* desktop);

static void ws_ctx_lock(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_lock(&desktop->ws_mutex);
}

static void ws_ctx_unlock(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_unlock(&desktop->ws_mutex);
}

static void ws_render_signal(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_lock(&desktop->render_mutex);
    (void) pthread_mutex_unlock(&desktop->render_mutex);
}

static bool ws_tiles_init(ws_desktop_t* desktop)
{
    if (!desktop)
        return false;

    uint32_t w = (desktop->ws.mode.hdisplay + WS_RENDER_TILE_W - 1U) / WS_RENDER_TILE_W;
    uint32_t h = (desktop->ws.mode.vdisplay + WS_RENDER_TILE_H - 1U) / WS_RENDER_TILE_H;
    if (w == 0U || h == 0U)
        return false;

    size_t count = (size_t) w * (size_t) h;
    uint8_t* tiles = (uint8_t*) calloc(count, sizeof(uint8_t));
    if (!tiles)
        return false;

    desktop->tiles_w = w;
    desktop->tiles_h = h;
    desktop->dirty_tiles = tiles;
    desktop->dirty_tile_count = 0U;
    return true;
}

static void ws_tiles_mark_all(ws_desktop_t* desktop)
{
    if (!desktop || !desktop->dirty_tiles)
        return;

    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    for (uint32_t i = 0U; i < total; i++)
        desktop->dirty_tiles[i] = 1U;
    desktop->dirty_tile_count = total;
}

static void ws_tiles_clear(ws_desktop_t* desktop)
{
    if (!desktop || !desktop->dirty_tiles)
        return;
    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    memset(desktop->dirty_tiles, 0, (size_t) total);
    desktop->dirty_tile_count = 0U;
}

static void ws_tiles_mark_rect(ws_desktop_t* desktop,
                               int32_t x,
                               int32_t y,
                               uint32_t width,
                               uint32_t height)
{
    if (!desktop || !desktop->dirty_tiles || width == 0U || height == 0U)
        return;

    int32_t max_w = (int32_t) desktop->ws.mode.hdisplay;
    int32_t max_h = (int32_t) desktop->ws.mode.vdisplay;
    if (max_w <= 0 || max_h <= 0)
        return;

    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + (int32_t) width;
    int32_t y1 = y + (int32_t) height;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > max_w)
        x1 = max_w;
    if (y1 > max_h)
        y1 = max_h;
    if (x0 >= x1 || y0 >= y1)
        return;

    uint32_t tx0 = (uint32_t) x0 / WS_RENDER_TILE_W;
    uint32_t ty0 = (uint32_t) y0 / WS_RENDER_TILE_H;
    uint32_t tx1 = (uint32_t) (x1 - 1) / WS_RENDER_TILE_W;
    uint32_t ty1 = (uint32_t) (y1 - 1) / WS_RENDER_TILE_H;

    if (tx1 >= desktop->tiles_w)
        tx1 = desktop->tiles_w - 1U;
    if (ty1 >= desktop->tiles_h)
        ty1 = desktop->tiles_h - 1U;

    for (uint32_t ty = ty0; ty <= ty1; ty++)
    {
        uint32_t row = ty * desktop->tiles_w;
        for (uint32_t tx = tx0; tx <= tx1; tx++)
        {
            uint32_t idx = row + tx;
            if (desktop->dirty_tiles[idx] == 0U)
            {
                desktop->dirty_tiles[idx] = 1U;
                desktop->dirty_tile_count++;
            }
        }
    }
}

static bool ws_tiles_pop_bbox(ws_desktop_t* desktop,
                              uint32_t budget,
                              int32_t* out_x,
                              int32_t* out_y,
                              uint32_t* out_w,
                              uint32_t* out_h)
{
    if (!desktop || !desktop->dirty_tiles || desktop->dirty_tile_count == 0U ||
        budget == 0U ||
        !out_x || !out_y || !out_w || !out_h)
        return false;

    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    bool found = false;
    int32_t bx0 = 0;
    int32_t by0 = 0;
    int32_t bx1 = 0;
    int32_t by1 = 0;
    uint32_t consumed = 0U;

    for (uint32_t idx = 0U; idx < total && consumed < budget; idx++)
    {
        if (desktop->dirty_tiles[idx] == 0U)
            continue;

        desktop->dirty_tiles[idx] = 0U;
        if (desktop->dirty_tile_count > 0U)
            desktop->dirty_tile_count--;

        uint32_t tx = idx % desktop->tiles_w;
        uint32_t ty = idx / desktop->tiles_w;
        int32_t x = (int32_t) (tx * WS_RENDER_TILE_W);
        int32_t y = (int32_t) (ty * WS_RENDER_TILE_H);
        uint32_t w = WS_RENDER_TILE_W;
        uint32_t h = WS_RENDER_TILE_H;
        if ((uint32_t) x + w > desktop->ws.mode.hdisplay)
            w = desktop->ws.mode.hdisplay - (uint32_t) x;
        if ((uint32_t) y + h > desktop->ws.mode.vdisplay)
            h = desktop->ws.mode.vdisplay - (uint32_t) y;
        if (!found)
        {
            bx0 = x;
            by0 = y;
            bx1 = x + (int32_t) w;
            by1 = y + (int32_t) h;
            found = true;
        }
        else
        {
            if (x < bx0)
                bx0 = x;
            if (y < by0)
                by0 = y;
            if (x + (int32_t) w > bx1)
                bx1 = x + (int32_t) w;
            if (y + (int32_t) h > by1)
                by1 = y + (int32_t) h;
        }
        consumed++;
    }

    if (!found)
        return false;

    if (bx0 < 0)
        bx0 = 0;
    if (by0 < 0)
        by0 = 0;
    if (bx1 < bx0 || by1 < by0)
        return false;

    *out_x = bx0;
    *out_y = by0;
    *out_w = (uint32_t) (bx1 - bx0);
    *out_h = (uint32_t) (by1 - by0);
    return (*out_w != 0U && *out_h != 0U);
}

static void* ws_render_thread_main(void* arg)
{
    ws_desktop_t* desktop = (ws_desktop_t*) arg;
    if (!desktop)
        return NULL;
    uint64_t dbg_epoch = 0ULL;
    uint64_t dbg_full_attempt = 0ULL;
    uint64_t dbg_region_attempt = 0ULL;
    uint64_t dbg_region_oor = 0ULL;

    for (;;)
    {
        ws_context_t snapshot;
        (void) pthread_mutex_lock(&desktop->render_mutex);
        bool do_full = desktop->render_full_pending;
        if (do_full)
            desktop->render_full_pending = false;
        bool has_tiles = desktop->dirty_tile_count > 0U;
        bool should_exit = desktop->render_thread_exit;
        (void) pthread_mutex_unlock(&desktop->render_mutex);

        if (should_exit)
            break;

        if (!do_full && !has_tiles)
        {
            (void) usleep(WS_RENDER_THREAD_SLEEP_US);
            continue;
        }

        ws_ctx_lock(desktop);
        memcpy(&snapshot, &desktop->ws, sizeof(snapshot));
        ws_ctx_unlock(desktop);

        uint64_t now_tick = sys_tick_get();
        uint64_t epoch = now_tick / 2000ULL;
        if (epoch != 0ULL && epoch > dbg_epoch)
        {
            dbg_epoch = epoch;
            // #region agent log
            WS_LOG("[AGENTDBG H22 RENDER_THREAD] tick=%llu full=%llu region=%llu oor=%llu ready=%u map=%p mode=%ux%u\n",
                   (unsigned long long) now_tick,
                   (unsigned long long) dbg_full_attempt,
                   (unsigned long long) dbg_region_attempt,
                   (unsigned long long) dbg_region_oor,
                   snapshot.ready ? 1U : 0U,
                   (void*) snapshot.frame_map,
                   (unsigned int) snapshot.mode.hdisplay,
                   (unsigned int) snapshot.mode.vdisplay);
            // #endregion
        }

        if (do_full)
        {
            dbg_full_attempt++;
            uint64_t render_start = sys_tick_get();
            int rc = ws_render(&snapshot);
            uint64_t render_end = sys_tick_get();

            (void) pthread_mutex_lock(&desktop->render_mutex);
            if (rc == 0)
            {
                desktop->stat_renders++;
                desktop->stat_render_ticks_total += ws_tick_delta(render_end, render_start);
                if (ws_tick_delta(render_end, render_start) > desktop->stat_render_ticks_max)
                    desktop->stat_render_ticks_max = ws_tick_delta(render_end, render_start);
            }
            else
            {
                desktop->stat_render_fail++;
            }
            (void) pthread_mutex_unlock(&desktop->render_mutex);
            continue;
        }

        uint64_t batch_start = sys_tick_get();
        for (;;)
        {
            int32_t rx = 0;
            int32_t ry = 0;
            uint32_t rw = 0U;
            uint32_t rh = 0U;

            (void) pthread_mutex_lock(&desktop->render_mutex);
            bool has_region = ws_tiles_pop_bbox(desktop, WS_RENDER_TILE_BUDGET, &rx, &ry, &rw, &rh);
            (void) pthread_mutex_unlock(&desktop->render_mutex);
            if (!has_region)
                break;

            dbg_region_attempt++;
            uint32_t mode_w = snapshot.mode.hdisplay;
            uint32_t mode_h = snapshot.mode.vdisplay;
            if (rw == 0U || rh == 0U ||
                rx < 0 || ry < 0 ||
                (uint32_t) rx >= mode_w || (uint32_t) ry >= mode_h ||
                rw > mode_w || rh > mode_h)
            {
                dbg_region_oor++;
            }
            uint64_t render_start = sys_tick_get();
            int rc = ws_render_region(&snapshot, rx, ry, rw, rh);
            uint64_t render_end = sys_tick_get();

            (void) pthread_mutex_lock(&desktop->render_mutex);
            if (rc == 0)
            {
                desktop->stat_renders++;
                desktop->stat_partial_renders++;
                uint64_t ticks = ws_tick_delta(render_end, render_start);
                desktop->stat_render_ticks_total += ticks;
                if (ticks > desktop->stat_render_ticks_max)
                    desktop->stat_render_ticks_max = ticks;
                if (ticks >= WS_RENDER_SLOW_TICKS)
                    desktop->stat_render_slow++;
            }
            else
            {
                desktop->stat_render_fail++;
            }
            (void) pthread_mutex_unlock(&desktop->render_mutex);

            uint64_t now = sys_tick_get();
            if (ws_tick_delta(now, batch_start) >= WS_RENDER_TIME_BUDGET_TICKS)
            {
                if (WS_RENDER_THREAD_YIELD_US != 0U)
                    (void) usleep(WS_RENDER_THREAD_YIELD_US);
                batch_start = now;
            }
        }
    }

    return NULL;
}

static void ws_dirty_region_reset(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    desktop->dirty_region_valid = false;
    desktop->dirty_x0 = 0;
    desktop->dirty_y0 = 0;
    desktop->dirty_x1 = 0;
    desktop->dirty_y1 = 0;
}

static void ws_dirty_region_add_rect(ws_desktop_t* desktop,
                                     int32_t x,
                                     int32_t y,
                                     uint32_t width,
                                     uint32_t height)
{
    if (!desktop || width == 0U || height == 0U)
        return;

    int32_t max_w = (int32_t) desktop->ws.mode.hdisplay;
    int32_t max_h = (int32_t) desktop->ws.mode.vdisplay;
    if (max_w <= 0 || max_h <= 0)
        return;

    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + (int32_t) width;
    int32_t y1 = y + (int32_t) height;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > max_w)
        x1 = max_w;
    if (y1 > max_h)
        y1 = max_h;
    if (x0 >= x1 || y0 >= y1)
        return;

    if (!desktop->dirty_region_valid)
    {
        desktop->dirty_region_valid = true;
        desktop->dirty_x0 = x0;
        desktop->dirty_y0 = y0;
        desktop->dirty_x1 = x1;
        desktop->dirty_y1 = y1;
        return;
    }

    if (x0 < desktop->dirty_x0)
        desktop->dirty_x0 = x0;
    if (y0 < desktop->dirty_y0)
        desktop->dirty_y0 = y0;
    if (x1 > desktop->dirty_x1)
        desktop->dirty_x1 = x1;
    if (y1 > desktop->dirty_y1)
        desktop->dirty_y1 = y1;
}

static bool ws_dirty_region_take(ws_desktop_t* desktop,
                                 int32_t* out_x,
                                 int32_t* out_y,
                                 uint32_t* out_width,
                                 uint32_t* out_height)
{
    if (!desktop || !out_x || !out_y || !out_width || !out_height || !desktop->dirty_region_valid)
        return false;

    int32_t x0 = desktop->dirty_x0;
    int32_t y0 = desktop->dirty_y0;
    int32_t x1 = desktop->dirty_x1;
    int32_t y1 = desktop->dirty_y1;
    if (x0 >= x1 || y0 >= y1)
    {
        ws_dirty_region_reset(desktop);
        return false;
    }

    *out_x = x0;
    *out_y = y0;
    *out_width = (uint32_t) (x1 - x0);
    *out_height = (uint32_t) (y1 - y0);
    ws_dirty_region_reset(desktop);
    return true;
}

static void ws_dirty_region_add_cursor_delta(ws_desktop_t* desktop,
                                             int32_t prev_cursor_x,
                                             int32_t prev_cursor_y,
                                             int32_t cursor_x,
                                             int32_t cursor_y)
{
    if (!desktop)
        return;

    int32_t box_w = (int32_t) WS_CURSOR_WIDTH + (int32_t) WS_CURSOR_SHADOW_OFFSET;
    int32_t box_h = (int32_t) WS_CURSOR_HEIGHT + (int32_t) WS_CURSOR_SHADOW_OFFSET;

    int32_t x0 = prev_cursor_x;
    int32_t y0 = prev_cursor_y;
    int32_t x1 = prev_cursor_x + box_w;
    int32_t y1 = prev_cursor_y + box_h;

    int32_t nx0 = cursor_x;
    int32_t ny0 = cursor_y;
    int32_t nx1 = cursor_x + box_w;
    int32_t ny1 = cursor_y + box_h;

    if (nx0 < x0)
        x0 = nx0;
    if (ny0 < y0)
        y0 = ny0;
    if (nx1 > x1)
        x1 = nx1;
    if (ny1 > y1)
        y1 = ny1;

    if (x0 >= x1 || y0 >= y1)
        return;

    ws_dirty_region_add_rect(desktop, x0, y0, (uint32_t) (x1 - x0), (uint32_t) (y1 - y0));
}

static void ws_desktop_invalidate_window_id(ws_desktop_t* desktop, uint32_t window_id)
{
    if (!desktop || window_id == 0U)
        return;

    ws_window_t w;
    if (ws_find_window(&desktop->ws, window_id, &w) == 0)
        ws_dirty_region_add_rect(desktop, w.x, w.y, w.width, w.height);
}

static uint32_t ws_text_count_lines(const char* text)
{
    if (!text || text[0] == '\0')
        return 0U;

    uint32_t lines = 1U;
    for (size_t i = 0U; text[i] != '\0'; i++)
    {
        if (text[i] == '\n')
            lines++;
    }
    return lines;
}

static uint32_t ws_text_line_index_at(const char* text, size_t index)
{
    if (!text)
        return 0U;

    uint32_t line = 0U;
    for (size_t i = 0U; i < index && text[i] != '\0'; i++)
    {
        if (text[i] == '\n')
            line++;
    }
    return line;
}

static bool ws_invalidate_window_text_delta(ws_desktop_t* desktop,
                                            const ws_window_t* window,
                                            const char* old_text,
                                            const char* new_text)
{
    if (!desktop || !window || !old_text || !new_text)
        return false;
    if (!desktop->ws.font_ready || desktop->ws.font_height == 0U)
        return false;

    if (strcmp(old_text, new_text) == 0)
        return true;

    size_t old_len = strlen(old_text);
    size_t new_len = strlen(new_text);
    size_t first_diff = 0U;
    while (first_diff < old_len &&
           first_diff < new_len &&
           old_text[first_diff] == new_text[first_diff])
        first_diff++;

    uint32_t start_line = ws_text_line_index_at(new_text, first_diff);
    uint32_t old_lines = ws_text_count_lines(old_text);
    uint32_t new_lines = ws_text_count_lines(new_text);
    uint32_t end_line = 0U;

    if (old_len != new_len || old_lines != new_lines)
    {
        uint32_t max_lines = (old_lines > new_lines) ? old_lines : new_lines;
        if (max_lines == 0U)
            return false;
        end_line = max_lines - 1U;
    }
    else
    {
        size_t old_end = old_len;
        size_t new_end = new_len;
        while (old_end > first_diff &&
               new_end > first_diff &&
               old_text[old_end - 1U] == new_text[new_end - 1U])
        {
            old_end--;
            new_end--;
        }
        if (new_end == 0U)
            return false;
        end_line = ws_text_line_index_at(new_text, new_end - 1U);
    }

    uint32_t line_h = desktop->ws.font_height + 2U;
    int32_t content_x = window->x + (int32_t) WS_WINDOW_BODY_PAD_X;
    int32_t content_y = window->y + (int32_t) WS_TITLEBAR_HEIGHT + (int32_t) WS_WINDOW_BODY_PAD_Y;
    int32_t content_y1 = window->y + (int32_t) window->height - (int32_t) WS_WINDOW_BODY_BOTTOM_MARGIN;
    if (content_y1 <= content_y)
        return false;

    uint32_t content_w = 0U;
    if (window->width > (WS_WINDOW_BODY_PAD_X * 2U))
        content_w = window->width - (WS_WINDOW_BODY_PAD_X * 2U);
    if (content_w == 0U || line_h == 0U)
        return false;

    int32_t y0 = content_y + (int32_t) (start_line * line_h);
    int32_t y1 = content_y + (int32_t) ((end_line + 1U) * line_h);
    if (y0 < content_y)
        y0 = content_y;
    if (y1 > content_y1)
        y1 = content_y1;
    if (y0 >= y1)
        return false;

    ws_dirty_region_add_rect(desktop,
                             content_x,
                             y0,
                             content_w,
                             (uint32_t) (y1 - y0));
    return true;
}

static void ws_desktop_invalidate_chrome(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    ws_window_t w;
    if (ws_find_window(&desktop->ws, desktop->top_bar_id, &w) == 0)
        ws_dirty_region_add_rect(desktop, w.x, w.y, w.width, w.height);
    if (ws_find_window(&desktop->ws, desktop->dock_bar_id, &w) == 0)
        ws_dirty_region_add_rect(desktop, w.x, w.y, w.width, w.height);
    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        if (ws_find_window(&desktop->ws, desktop->dock_icon_ids[i], &w) == 0)
            ws_dirty_region_add_rect(desktop, w.x, w.y, w.width, w.height);
    }
}

static uint32_t ws_active_client_count(const ws_desktop_t* desktop)
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

static uint32_t ws_active_client_windows(const ws_desktop_t* desktop)
{
    if (!desktop)
        return 0U;

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        count += desktop->clients[i].window_count;

    return count;
}

static const char* ws_launch_cmd_name(ws_launch_cmd_t cmd)
{
    switch (cmd)
    {
        case WS_LAUNCH_CMD_SHELL_GUI:
            return "shell-gui";
        case WS_LAUNCH_CMD_SYSTEM_MONITOR_GUI:
            return "monitor-gui";
        case WS_LAUNCH_CMD_POWER_SHUTDOWN:
            return "power-shutdown";
        case WS_LAUNCH_CMD_POWER_RESTART:
            return "power-restart";
        default:
            return "none";
    }
}

static void ws_client_reset_slot(ws_client_conn_t* client)
{
    if (!client)
        return;

    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->role = WS_CLIENT_ROLE_GENERIC;
}

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

static int ws_create_window_checked(ws_context_t* ws, const ws_window_desc_t* desc, uint32_t* out_id)
{
    if (ws_create_window(ws, desc, out_id) < 0)
        return -1;
    return 0;
}

static bool ws_window_fetch(const ws_context_t* ws, uint32_t window_id, ws_window_t* out)
{
    if (!ws || window_id == 0U || !out)
        return false;
    return ws_find_window(ws, window_id, out) == 0;
}

static bool ws_point_in_window_raw(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!window || !window->visible)
        return false;

    int32_t x0 = window->x;
    int32_t y0 = window->y;
    int32_t x1 = x0 + (int32_t) window->width;
    int32_t y1 = y0 + (int32_t) window->height;
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static bool ws_point_in_titlebar(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!ws_point_in_window_raw(window, x, y))
        return false;

    int32_t y1 = window->y + (int32_t) WS_TITLEBAR_HEIGHT;
    return y >= window->y && y < y1;
}

static bool ws_point_in_close_button(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!window || !window->frame_controls)
        return false;
    if (!ws_point_in_titlebar(window, x, y))
        return false;
    if (window->width <= (WS_CLOSE_BUTTON_SIZE + (WS_CLOSE_BUTTON_MARGINX * 2U)))
        return false;

    int32_t bx = window->x + (int32_t) window->width - (int32_t) WS_CLOSE_BUTTON_MARGINX - (int32_t) WS_CLOSE_BUTTON_SIZE;
    int32_t by = window->y + (int32_t) WS_CLOSE_BUTTON_MARGINY;
    int32_t bx2 = bx + (int32_t) WS_CLOSE_BUTTON_SIZE;
    int32_t by2 = by + (int32_t) WS_CLOSE_BUTTON_SIZE;
    return x >= bx && x < bx2 && y >= by && y < by2;
}

static bool ws_client_add_window(ws_client_conn_t* client, uint32_t window_id)
{
    if (!client || window_id == 0U)
        return false;
    if (client->window_count >= WS_MAX_WINDOWS)
        return false;

    client->window_ids[client->window_count++] = window_id;
    return true;
}

static void ws_client_remove_window(ws_client_conn_t* client, uint32_t window_id)
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

static bool ws_client_owns_window(const ws_client_conn_t* client, uint32_t window_id)
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

static bool ws_find_client_by_window(const ws_desktop_t* desktop, uint32_t window_id, uint32_t* out_slot)
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

static bool ws_client_event_push(ws_client_conn_t* client, const ws_event_t* event)
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

static bool ws_client_event_pop(ws_client_conn_t* client, ws_event_t* out_event)
{
    if (!client || !out_event || client->event_count == 0U)
        return false;

    *out_event = client->events[client->event_head];
    client->event_head = (client->event_head + 1U) % WS_CLIENT_EVENT_QUEUE;
    client->event_count--;
    return true;
}

static void ws_client_destroy_all_windows(ws_desktop_t* desktop, ws_client_conn_t* client)
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

static void ws_client_disconnect_slot(ws_desktop_t* desktop, uint32_t slot)
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

static int ws_server_socket_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        WS_LOG("server socket() failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) WS_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (const struct sockaddr*) &addr, (socklen_t) sizeof(addr)) < 0)
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

    WS_LOG("server listening on 127.0.0.1:%u fd=%d\n",
           (unsigned int) WS_SERVER_PORT,
           fd);
    return fd;
}

static int ws_send_all_fd(int fd, const void* data, size_t len)
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
                (void) usleep(500U);
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

static ws_client_role_t ws_role_from_u32(uint32_t raw)
{
    if (raw == (uint32_t) WS_CLIENT_ROLE_SHELL_GUI)
        return WS_CLIENT_ROLE_SHELL_GUI;
    if (raw == (uint32_t) WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI)
        return WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI;
    return WS_CLIENT_ROLE_GENERIC;
}


static void ws_fill_response_base(const ws_ipc_packet_t* request, ws_ipc_packet_t* response)
{
    memset(response, 0, sizeof(*response));
    response->magic = WS_IPC_MAGIC;
    response->version = WS_IPC_VERSION;
    if (request)
        response->opcode = request->opcode;
}

static void ws_handle_client_request(ws_desktop_t* desktop,
                                     uint32_t slot,
                                     const ws_ipc_packet_t* request,
                                     ws_ipc_packet_t* response,
                                     bool* out_changed)
{
    if (!desktop || slot >= WS_CLIENT_MAX || !request || !response)
        return;

    ws_client_conn_t* client = &desktop->clients[slot];
    ws_fill_response_base(request, response);
    desktop->stat_requests++;
    if (out_changed)
        *out_changed = false;

    if (request->magic != WS_IPC_MAGIC || request->version != WS_IPC_VERSION)
    {
        WS_LOG("bad ipc packet slot=%u fd=%d magic=0x%08X version=%u\n",
               (unsigned int) slot,
               client->fd,
               (unsigned int) request->magic,
               (unsigned int) request->version);
        response->status = EIO;
        return;
    }

    if (WS_IPC_TRACE)
    {
        WS_LOG("request slot=%u pid=%u opcode=%u win=%u\n",
               (unsigned int) slot,
               (unsigned int) client->pid,
               (unsigned int) request->opcode,
               (unsigned int) request->window_id);
    }

    switch ((ws_ipc_opcode_t) request->opcode)
    {
        case WS_IPC_OP_HELLO:
        {
            client->pid = request->owner_pid;
            client->role = ws_role_from_u32(request->client_role);
            WS_LOG("hello slot=%u fd=%d pid=%u role=%u\n",
                   (unsigned int) slot,
                   client->fd,
                   (unsigned int) client->pid,
                   (unsigned int) client->role);
            response->status = 0;
            return;
        }

        case WS_IPC_OP_CREATE_WINDOW:
        {
            ws_window_desc_t desc;
            memset(&desc, 0, sizeof(desc));
            desc.x = request->x;
            desc.y = request->y;
            desc.width = request->width;
            desc.height = request->height;
            desc.color = request->color;
            desc.border_color = request->border_color;
            desc.titlebar_color = request->titlebar_color;
            desc.visible = request->visible != 0U;
            desc.frame_controls = request->frame_controls != 0U;
            desc.title = request->title;
            char safe_title[WS_WINDOW_TITLE_MAX + 1U];
            ws_copy_cstr_bounded(safe_title, sizeof(safe_title), request->title);
            WS_LOG("create-window req slot=%u pid=%u x=%d y=%d w=%u h=%u title='%s'\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   desc.x,
                   desc.y,
                   (unsigned int) desc.width,
                   (unsigned int) desc.height,
                   safe_title);

            uint32_t window_id = 0U;
            ws_ctx_lock(desktop);
            int rc_create = ws_create_window(&desktop->ws, &desc, &window_id);
            ws_ctx_unlock(desktop);
            if (rc_create < 0)
            {
                desktop->stat_req_create_fail++;
                WS_LOG("create-window failed slot=%u pid=%u errno=%d w=%u h=%u x=%d y=%d title='%s'\n",
                       (unsigned int) slot,
                       (unsigned int) client->pid,
                       errno,
                       (unsigned int) desc.width,
                       (unsigned int) desc.height,
                       desc.x,
                       desc.y,
                       safe_title);
                response->status = errno;
                return;
            }

            if (!ws_client_add_window(client, window_id))
            {
                ws_ctx_lock(desktop);
                (void) ws_destroy_window(&desktop->ws, window_id);
                ws_ctx_unlock(desktop);
                desktop->stat_req_create_fail++;
                WS_LOG("create-window add-owner failed slot=%u pid=%u win=%u\n",
                       (unsigned int) slot,
                       (unsigned int) client->pid,
                       (unsigned int) window_id);
                response->status = ENOSPC;
                return;
            }

            ws_ctx_lock(desktop);
            (void) ws_raise_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            ws_set_focus(desktop, window_id, slot);
            desktop->compositor_need_full = true;
            desktop->stat_req_create_ok++;
            WS_LOG("create-window ok slot=%u pid=%u win=%u title='%s'\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   (unsigned int) window_id,
                   safe_title);
            response->window_id = window_id;
            response->status = 0;
            ws_desktop_invalidate_window_id(desktop, window_id);
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_DESTROY_WINDOW:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_destroy = ws_destroy_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            if (rc_destroy < 0)
            {
                response->status = errno;
                return;
            }

            ws_client_remove_window(client, window_id);
            WS_LOG("destroy-window slot=%u pid=%u win=%u\n",
                   (unsigned int) slot,
                   (unsigned int) client->pid,
                   (unsigned int) window_id);
            if (desktop->focused_window_id == window_id)
                ws_refresh_focus_after_close(desktop);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_MOVE_WINDOW:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_move = ws_move_window(&desktop->ws, window_id, request->x, request->y);
            ws_ctx_unlock(desktop);
            if (rc_move < 0)
            {
                response->status = errno;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_RAISE_WINDOW:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            uint32_t top_id = 0U;
            if (desktop->ws.window_count > 0U)
                top_id = desktop->ws.windows[desktop->ws.window_count - 1U].id;
            if (window_id == top_id)
            {
                response->status = 0;
                return;
            }

            ws_ctx_lock(desktop);
            int rc_raise = ws_raise_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            if (rc_raise < 0)
            {
                response->status = errno;
                return;
            }

            desktop->compositor_need_full = true;
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_RESIZE_WINDOW:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            ws_ctx_lock(desktop);
            int rc_resize = ws_resize_window(&desktop->ws, window_id, request->width, request->height);
            ws_ctx_unlock(desktop);
            if (rc_resize < 0)
            {
                response->status = errno;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_COLOR:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_ctx_lock(desktop);
            int rc_color = ws_set_window_color(&desktop->ws, window_id, request->color);
            ws_ctx_unlock(desktop);
            if (rc_color < 0)
            {
                response->status = errno;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_VISIBLE:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_ctx_lock(desktop);
            int rc_visible = ws_set_window_visible(&desktop->ws, window_id, request->visible != 0U);
            ws_ctx_unlock(desktop);
            if (rc_visible < 0)
            {
                response->status = errno;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_TITLE:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_ctx_lock(desktop);
            int rc_title = ws_set_window_title(&desktop->ws, window_id, request->title);
            ws_ctx_unlock(desktop);
            if (rc_title < 0)
            {
                response->status = errno;
                return;
            }

            ws_desktop_invalidate_window_id(desktop, window_id);
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_SET_WINDOW_TEXT:
        {
            uint32_t window_id = request->window_id;
            if (!ws_client_owns_window(client, window_id))
            {
                response->status = EPERM;
                return;
            }

            ws_window_t window_before;
            ws_ctx_lock(desktop);
            bool have_window = ws_find_window(&desktop->ws, window_id, &window_before) == 0;
            ws_ctx_unlock(desktop);
            if (have_window && strcmp(window_before.body_text, request->text) == 0)
            {
                desktop->stat_set_text_noop++;
                response->status = 0;
                return;
            }

            desktop->stat_set_text++;
            ws_ctx_lock(desktop);
            int rc_text = ws_set_window_text(&desktop->ws, window_id, request->text);
            ws_ctx_unlock(desktop);
            if (rc_text < 0)
            {
                response->status = errno;
                return;
            }

            if (!have_window || !ws_invalidate_window_text_delta(desktop,
                                                                 &window_before,
                                                                 window_before.body_text,
                                                                 request->text))
            {
                ws_desktop_invalidate_window_id(desktop, window_id);
            }
            response->status = 0;
            if (out_changed)
                *out_changed = true;
            return;
        }

        case WS_IPC_OP_POLL_EVENT:
        {
            desktop->stat_poll_event++;
            ws_event_t event;
            uint32_t queue_before = client->event_count;
            if (!ws_client_event_pop(client, &event))
            {
                desktop->stat_poll_eagain++;
                response->status = EAGAIN;
                return;
            }

            response->status = 0;
            response->window_id = event.window_id;
            response->event_type = (uint32_t) event.type;
            response->event_key = event.key;
            if (queue_before > 16U)
            {
                // #region agent log
                WS_LOG("[AGENTDBG H30 POLL_QDEPTH] slot=%u before=%u after=%u event=%u win=%u\n",
                       (unsigned int) slot,
                       (unsigned int) queue_before,
                       (unsigned int) client->event_count,
                       (unsigned int) event.type,
                       (unsigned int) event.window_id);
                // #endregion
            }
            return;
        }

        default:
            response->status = ENOTSUP;
            return;
    }
}

static bool ws_accept_clients(ws_desktop_t* desktop)
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

static bool ws_poll_clients(ws_desktop_t* desktop, bool* out_activity)
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
            if (client->rx_used >= sizeof(ws_ipc_packet_t))
                client->rx_used = 0U;

            ssize_t rc = recv(client->fd,
                              client->rx_buf + client->rx_used,
                              sizeof(ws_ipc_packet_t) - client->rx_used,
                              MSG_DONTWAIT);
            if (rc < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;

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
                WS_LOG("recv EOF slot=%u fd=%d\n", (unsigned int) i, client->fd);
                ws_client_disconnect_slot(desktop, i);
                changed = true;
                break;
            }

            client->rx_used += (size_t) rc;
            if (client->rx_used < sizeof(ws_ipc_packet_t))
                continue;

            ws_ipc_packet_t request;
            memcpy(&request, client->rx_buf, sizeof(request));
            request.title[WS_WINDOW_TITLE_MAX] = '\0';
            request.text[WS_WINDOW_BODY_TEXT_MAX] = '\0';
            client->rx_used = 0U;
            processed_requests++;

            ws_ipc_packet_t response;
            bool request_changed = false;
            ws_handle_client_request(desktop, i, &request, &response, &request_changed);
            if (request_changed)
                changed = true;
            if (request_changed || request.opcode != WS_IPC_OP_POLL_EVENT || response.status == 0)
                activity = true;

            if (ws_send_all_fd(client->fd, &response, sizeof(response)) < 0)
            {
                desktop->stat_send_fail++;
                WS_LOG("send response failed slot=%u fd=%d opcode=%u errno=%d\n",
                       (unsigned int) i,
                       client->fd,
                       (unsigned int) request.opcode,
                       errno);
                ws_client_disconnect_slot(desktop, i);
                changed = true;
                keep_running = false;
            }
        }
    }

    if (out_activity)
        *out_activity = activity;
    return changed;
}

static bool ws_role_has_live_client(const ws_desktop_t* desktop, ws_client_role_t role)
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

static bool ws_raise_first_window_for_role(ws_desktop_t* desktop, ws_client_role_t role)
{
    if (!desktop)
        return false;

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
    {
        ws_client_conn_t* client = &desktop->clients[i];
        if (!client->active)
            continue;
        if (client->role != role)
            continue;
        if (client->window_count == 0U)
            continue;

        uint32_t window_id = client->window_ids[client->window_count - 1U];
        uint32_t top_id = 0U;
        if (desktop->ws.window_count > 0U)
            top_id = desktop->ws.windows[desktop->ws.window_count - 1U].id;
        if (window_id == top_id)
            return true;
        ws_ctx_lock(desktop);
        int rc_raise = ws_raise_window(&desktop->ws, window_id);
        ws_ctx_unlock(desktop);
        if (rc_raise != 0)
            return false;
        desktop->compositor_need_full = true;
        return true;
    }

    return false;
}

static void ws_refresh_launch_pending(ws_desktop_t* desktop, uint64_t now_tick)
{
    if (!desktop)
        return;

    if (desktop->shell_launch_pending)
    {
        bool shell_live = ws_role_has_live_client(desktop, WS_CLIENT_ROLE_SHELL_GUI);
        if (shell_live ||
            (now_tick - desktop->shell_launch_tick) >= WS_LAUNCH_PENDING_TIMEOUT_TICKS)
        {
            desktop->shell_launch_pending = false;
        }
    }

    if (desktop->monitor_launch_pending)
    {
        bool monitor_live = ws_role_has_live_client(desktop, WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI);
        if (monitor_live ||
            (now_tick - desktop->monitor_launch_tick) >= WS_LAUNCH_PENDING_TIMEOUT_TICKS)
        {
            desktop->monitor_launch_pending = false;
        }
    }
}

static void ws_launcher_reap_children(void)
{
    /* Bound reaping work to avoid monopolizing the launcher loop if waitpid misbehaves. */
    for (uint32_t i = 0U; i < 8U; i++)
    {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            return;
    }
}

static int ws_launcher_spawn_exec(const char* path, const char* name, const char* arg)
{
    if (!path || !name)
    {
        errno = EINVAL;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        WS_LOG("launcher fork failed path='%s' errno=%d\n", path, errno);
        return -1;
    }

    if (pid == 0)
    {
        if (arg && arg[0] != '\0')
        {
            char* const argv[] = {
                (char*) name,
                (char*) arg,
                NULL
            };
            (void) execv(path, argv);
        }
        else
        {
            char* const argv[] = {
                (char*) name,
                NULL
            };
            (void) execv(path, argv);
        }
        _exit(127);
    }

    WS_LOG("launcher spawned pid=%d path='%s' arg='%s'\n",
           (int) pid,
           path,
           (arg && arg[0] != '\0') ? arg : "");
    return 0;
}

static void ws_launcher_handle_command(ws_launch_cmd_t cmd)
{
    switch (cmd)
    {
        case WS_LAUNCH_CMD_SHELL_GUI:
            (void) ws_launcher_spawn_exec("/bin/TheShellGUI", "TheShellGUI", NULL);
            return;

        case WS_LAUNCH_CMD_SYSTEM_MONITOR_GUI:
            (void) ws_launcher_spawn_exec("/bin/TheSystemMonitorGUI", "TheSystemMonitorGUI", NULL);
            return;

        case WS_LAUNCH_CMD_POWER_SHUTDOWN:
            (void) ws_launcher_spawn_exec("/bin/ThePowerManager", "ThePowerManager", "-s");
            return;

        case WS_LAUNCH_CMD_POWER_RESTART:
            (void) ws_launcher_spawn_exec("/bin/ThePowerManager", "ThePowerManager", "-r");
            return;

        default:
            return;
    }
}

static int ws_launcher_open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) WS_LAUNCHER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (const struct sockaddr*) &addr, (socklen_t) sizeof(addr)) < 0)
    {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    if (ws_set_socket_non_blocking(fd) < 0)
    {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static void ws_launcher_main(void)
{
    int fd = ws_launcher_open_socket();
    if (fd < 0)
        _exit(1);

    WS_LOG("launcher ready port=%u fd=%d\n",
           (unsigned int) WS_LAUNCHER_PORT,
           fd);

    for (;;)
    {
        ws_launcher_reap_children();

        ws_launch_msg_t msg;
        ssize_t rc = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                (void) usleep(1000U);
                continue;
            }
            (void) usleep(1000U);
            continue;
        }

        if ((size_t) rc < sizeof(msg))
            continue;
        if (msg.magic != WS_LAUNCHER_MAGIC)
            continue;

        WS_LOG("launcher recv cmd=%s(%u)\n",
               ws_launch_cmd_name((ws_launch_cmd_t) msg.cmd),
               (unsigned int) msg.cmd);
        ws_launcher_handle_command((ws_launch_cmd_t) msg.cmd);
    }
}

static pid_t ws_launcher_start(void)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        WS_LOG("launcher start fork failed errno=%d\n", errno);
        return (pid_t) -1;
    }
    if (pid == 0)
        ws_launcher_main();
    WS_LOG("launcher process started pid=%d\n", (int) pid);
    return pid;
}

static int ws_launcher_send_command(ws_launch_cmd_t cmd)
{
    if (cmd == WS_LAUNCH_CMD_NONE)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        WS_LOG("launcher send socket failed cmd=%s errno=%d\n",
               ws_launch_cmd_name(cmd),
               errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) WS_LAUNCHER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ws_launch_msg_t msg;
    msg.magic = WS_LAUNCHER_MAGIC;
    msg.cmd = (uint32_t) cmd;

    ssize_t rc = sendto(fd,
                        &msg,
                        sizeof(msg),
                        0,
                        (const struct sockaddr*) &addr,
                        (socklen_t) sizeof(addr));
    int saved_errno = errno;
    (void) close(fd);

    if (rc != (ssize_t) sizeof(msg))
    {
        errno = (rc < 0) ? saved_errno : EIO;
        WS_LOG("launcher send failed cmd=%s rc=%d errno=%d\n",
               ws_launch_cmd_name(cmd),
               (int) rc,
               errno);
        return -1;
    }

    WS_LOG("launcher send ok cmd=%s\n", ws_launch_cmd_name(cmd));
    return 0;
}

static int ws_launch_powermanager(const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(arg, "-s") == 0)
    {
        if (ws_launcher_send_command(WS_LAUNCH_CMD_POWER_SHUTDOWN) == 0)
            return 0;
        int send_errno = errno;
        WS_LOG("[AGENTDBG H36 LAUNCH_FALLBACK] cmd=power-shutdown send_errno=%d\n", send_errno);
        if (ws_launcher_spawn_exec("/bin/ThePowerManager", "ThePowerManager", "-s") == 0)
            return 0;
        return -1;
    }
    if (strcmp(arg, "-r") == 0)
    {
        if (ws_launcher_send_command(WS_LAUNCH_CMD_POWER_RESTART) == 0)
            return 0;
        int send_errno = errno;
        WS_LOG("[AGENTDBG H36 LAUNCH_FALLBACK] cmd=power-restart send_errno=%d\n", send_errno);
        if (ws_launcher_spawn_exec("/bin/ThePowerManager", "ThePowerManager", "-r") == 0)
            return 0;
        return -1;
    }

    errno = EINVAL;
    return -1;
}

static int ws_launch_gui_app(const char* path, const char* name)
{
    (void) name;
    if (!path)
    {
        errno = EINVAL;
        return -1;
    }

    if (strcmp(path, "/bin/TheShellGUI") == 0)
    {
        if (ws_launcher_send_command(WS_LAUNCH_CMD_SHELL_GUI) == 0)
            return 0;
        int send_errno = errno;
        WS_LOG("[AGENTDBG H36 LAUNCH_FALLBACK] cmd=shell-gui send_errno=%d\n", send_errno);
        if (ws_launcher_spawn_exec("/bin/TheShellGUI", "TheShellGUI", NULL) == 0)
            return 0;
        return -1;
    }
    if (strcmp(path, "/bin/TheSystemMonitorGUI") == 0)
    {
        if (ws_launcher_send_command(WS_LAUNCH_CMD_SYSTEM_MONITOR_GUI) == 0)
            return 0;
        int send_errno = errno;
        WS_LOG("[AGENTDBG H36 LAUNCH_FALLBACK] cmd=monitor-gui send_errno=%d\n", send_errno);
        if (ws_launcher_spawn_exec("/bin/TheSystemMonitorGUI", "TheSystemMonitorGUI", NULL) == 0)
            return 0;
        return -1;
    }

    errno = ENOTSUP;
    return -1;
}

static void ws_set_power_menu_visible(ws_desktop_t* desktop, bool visible)
{
    if (!desktop)
        return;

    ws_ctx_lock(desktop);
    desktop->power_menu_open = visible;
    (void) ws_set_window_visible(&desktop->ws, desktop->power_dimmer_id, visible);
    (void) ws_set_window_visible(&desktop->ws, desktop->power_panel_id, visible);
    for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
        (void) ws_set_window_visible(&desktop->ws, desktop->power_button_ids[i], visible);

    if (visible)
    {
        (void) ws_raise_window(&desktop->ws, desktop->power_dimmer_id);
        (void) ws_raise_window(&desktop->ws, desktop->power_panel_id);
        for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
            (void) ws_raise_window(&desktop->ws, desktop->power_button_ids[i]);
    }
    else
    {
        desktop->power_focus = WS_POWER_ITEM_SHUTDOWN;
    }

    desktop->compositor_need_full = true;
    desktop->colors_dirty = true;
    ws_ctx_unlock(desktop);
}

static void ws_execute_power_action(ws_desktop_t* desktop, ws_power_item_t action)
{
    if (!desktop)
        return;

    if (action == WS_POWER_ITEM_CANCEL)
    {
        ws_set_power_menu_visible(desktop, false);
        return;
    }

    if (action == WS_POWER_ITEM_SHUTDOWN)
    {
        desktop->stat_launcher_cmd_sent++;
        WS_LOG("power action: shutdown requested\n");
        if (ws_launch_powermanager("-s") < 0)
        {
            desktop->stat_launcher_cmd_send_fail++;
            WS_LOG("power action shutdown launcher failed errno=%d (fallback shutdown)\n", errno);
            (void) shutdown();
        }
        return;
    }

    if (action == WS_POWER_ITEM_RESTART)
    {
        desktop->stat_launcher_cmd_sent++;
        WS_LOG("power action: restart requested\n");
        if (ws_launch_powermanager("-r") < 0)
        {
            desktop->stat_launcher_cmd_send_fail++;
            WS_LOG("power action restart launcher failed errno=%d (fallback reboot)\n", errno);
            (void) reboot();
        }
    }
}

static void ws_apply_desktop_colors(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    (void) ws_set_window_color(&desktop->ws, desktop->top_bar_id, 0x003E5F7FU);
    (void) ws_set_window_color(&desktop->ws, desktop->dock_bar_id, 0x00B2C6D8U);

    bool shell_active = ws_role_has_live_client(desktop, WS_CLIENT_ROLE_SHELL_GUI);
    bool monitor_active = ws_role_has_live_client(desktop, WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI);

    const uint32_t base[WS_DOCK_ICON_COUNT] = {
        0x004C7AA0U,
        0x00488B7EU,
        0x00A86E6EU
    };
    const uint32_t active[WS_DOCK_ICON_COUNT] = {
        0x00629BCFU,
        0x005CB9A8U,
        0x00CC8080U
    };

    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        bool is_active = false;
        if (i == WS_DOCK_ITEM_SHELL && shell_active)
            is_active = true;
        if (i == WS_DOCK_ITEM_SYSTEM_MONITOR && monitor_active)
            is_active = true;
        if (i == WS_DOCK_ITEM_POWER && desktop->power_menu_open)
            is_active = true;

        (void) ws_set_window_color(&desktop->ws, desktop->dock_icon_ids[i], is_active ? active[i] : base[i]);
    }

    if (desktop->power_menu_open)
    {
        (void) ws_set_window_color(&desktop->ws, desktop->power_dimmer_id, 0x002A3C4DU);
        (void) ws_set_window_color(&desktop->ws, desktop->power_panel_id, 0x00D7E1EBU);

        const uint32_t power_base[WS_POWER_ITEM_COUNT] = {
            0x00AA6F6FU,
            0x007D98BBU,
            0x007893A8U
        };
        const uint32_t power_focus[WS_POWER_ITEM_COUNT] = {
            0x00D78383U,
            0x00A8C0E3U,
            0x00A2B8CBU
        };

        for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
        {
            uint32_t color = (desktop->power_focus == (ws_power_item_t) i) ? power_focus[i] : power_base[i];
            (void) ws_set_window_color(&desktop->ws, desktop->power_button_ids[i], color);
        }
    }
}

static bool ws_handle_power_click(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop || !desktop->power_menu_open)
        return false;

    for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
    {
        ws_window_t w;
        if (!ws_window_fetch(&desktop->ws, desktop->power_button_ids[i], &w))
            continue;
        if (!ws_point_in_window_raw(&w, x, y))
            continue;

        desktop->power_focus = (ws_power_item_t) i;
        desktop->colors_dirty = true;
        for (uint32_t j = 0U; j < WS_POWER_ITEM_COUNT; j++)
            ws_desktop_invalidate_window_id(desktop, desktop->power_button_ids[j]);
        ws_execute_power_action(desktop, desktop->power_focus);
        return true;
    }

    ws_window_t panel;
    if (ws_window_fetch(&desktop->ws, desktop->power_panel_id, &panel) && ws_point_in_window_raw(&panel, x, y))
        return true;

    ws_set_power_menu_visible(desktop, false);
    return true;
}

static void ws_handle_dock_click(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop)
        return;

    uint64_t now_tick = sys_tick_get();
    ws_refresh_launch_pending(desktop, now_tick);

    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        ws_window_t icon;
        if (!ws_window_fetch(&desktop->ws, desktop->dock_icon_ids[i], &icon))
            continue;
        if (!ws_point_in_window_raw(&icon, x, y))
            continue;

        if (i == WS_DOCK_ITEM_SHELL)
        {
            desktop->stat_launcher_cmd_sent++;
            WS_LOG("dock click: launch TheShellGUI\n");
            if (ws_launch_gui_app("/bin/TheShellGUI", "TheShellGUI") < 0)
            {
                desktop->stat_launcher_cmd_send_fail++;
                WS_LOG("dock launch TheShellGUI failed errno=%d\n", errno);
            }
            else
            {
                ws_desktop_invalidate_chrome(desktop);
            }
        }
        else if (i == WS_DOCK_ITEM_SYSTEM_MONITOR)
        {
            desktop->stat_launcher_cmd_sent++;
            WS_LOG("dock click: launch TheSystemMonitorGUI\n");
            if (ws_launch_gui_app("/bin/TheSystemMonitorGUI", "TheSystemMonitorGUI") < 0)
            {
                desktop->stat_launcher_cmd_send_fail++;
                WS_LOG("dock launch TheSystemMonitorGUI failed errno=%d\n", errno);
            }
            else
            {
                ws_desktop_invalidate_chrome(desktop);
            }
        }
        else if (i == WS_DOCK_ITEM_POWER)
        {
            WS_LOG("dock click: open power menu\n");
            ws_set_power_menu_visible(desktop, true);
        }

        return;
    }
}

static bool ws_pick_top_client_window_at(const ws_desktop_t* desktop,
                                         int32_t x,
                                         int32_t y,
                                         uint32_t* out_window_id,
                                         ws_window_t* out_window,
                                         uint32_t* out_slot)
{
    if (!desktop || !out_window_id || !out_window || !out_slot)
        return false;

    for (uint32_t i = desktop->ws.window_count; i > 0U; i--)
    {
        ws_window_t window = desktop->ws.windows[i - 1U];
        if (!ws_point_in_window_raw(&window, x, y))
            continue;

        uint32_t slot = 0U;
        if (!ws_find_client_by_window(desktop, window.id, &slot))
            continue;

        *out_window_id = window.id;
        *out_window = window;
        *out_slot = slot;
        return true;
    }

    return false;
}

static bool ws_pick_top_client_window(const ws_desktop_t* desktop,
                                      uint32_t* out_window_id,
                                      ws_window_t* out_window,
                                      uint32_t* out_slot)
{
    if (!desktop || !out_window_id || !out_window || !out_slot)
        return false;

    for (uint32_t i = desktop->ws.window_count; i > 0U; i--)
    {
        ws_window_t window = desktop->ws.windows[i - 1U];
        uint32_t slot = 0U;
        if (!ws_find_client_by_window(desktop, window.id, &slot))
            continue;

        *out_window_id = window.id;
        *out_window = window;
        *out_slot = slot;
        return true;
    }

    return false;
}

static void ws_set_focus(ws_desktop_t* desktop, uint32_t window_id, uint32_t slot)
{
    if (!desktop)
        return;

    desktop->focused_window_id = window_id;
    desktop->focused_slot = slot;
}

static void ws_clear_focus(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    desktop->focused_window_id = 0U;
    desktop->focused_slot = 0U;
}

static void ws_refresh_focus_after_close(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    if (desktop->focused_window_id == 0U)
        return;

    uint32_t window_id = 0U;
    uint32_t slot = 0U;
    ws_window_t window;
    if (ws_pick_top_client_window(desktop, &window_id, &window, &slot))
    {
        ws_set_focus(desktop, window_id, slot);
        return;
    }

    ws_clear_focus(desktop);
}

static void ws_dispatch_key_events(ws_desktop_t* desktop)
{
    static uint64_t ws_dbg_key_scancode_read = 0ULL;
    static uint64_t ws_dbg_key_push_ok = 0ULL;
    static uint64_t ws_dbg_key_push_fail = 0ULL;
    static uint64_t ws_dbg_key_no_focus = 0ULL;
    static uint64_t ws_dbg_key_skip_shellgui = 0ULL;
    static uint64_t ws_dbg_key_skip_shellgui_live = 0ULL;
    static uint64_t ws_dbg_key_epoch = 0ULL;
    static uint64_t ws_dbg_focus_epoch = 0ULL;
    static uint32_t ws_dbg_last_scancode = 0U;
    uint64_t now_tick = sys_tick_get();
    uint64_t epoch = now_tick / 500ULL;

    if (!desktop || desktop->focused_window_id == 0U)
    {
        ws_dbg_key_no_focus++;
        return;
    }

    if (desktop->focused_slot >= WS_CLIENT_MAX)
        return;

    ws_client_conn_t* client = &desktop->clients[desktop->focused_slot];
    bool owns_focused = ws_client_owns_window(client, desktop->focused_window_id);
    static uint64_t ws_dbg_focus_mismatch = 0ULL;
    if (!client->active || !owns_focused)
    {
        if (client->active && !owns_focused)
            ws_dbg_focus_mismatch++;
        if (epoch != 0ULL && epoch > ws_dbg_key_epoch)
        {
            ws_dbg_key_epoch = epoch;
            WS_LOG("[AGENTDBG H38 FOCUS_OWNER] tick=%llu focused_win=%u slot=%u active=%u owns=%u role=%u mismatch=%llu\n",
                   (unsigned long long) now_tick,
                   (unsigned int) desktop->focused_window_id,
                   (unsigned int) desktop->focused_slot,
                   client->active ? 1U : 0U,
                   owns_focused ? 1U : 0U,
                   (unsigned int) client->role,
                   (unsigned long long) ws_dbg_focus_mismatch);
        }
        return;
    }
    uint64_t focus_epoch = now_tick / 500ULL;
    if (focus_epoch != 0ULL && focus_epoch > ws_dbg_focus_epoch)
    {
        ws_dbg_focus_epoch = focus_epoch;
        WS_LOG("[AGENTDBG H37 FOCUS_STATE] tick=%llu focused_win=%u slot=%u role=%u active=%u owns=%u\n",
               (unsigned long long) now_tick,
               (unsigned int) desktop->focused_window_id,
               (unsigned int) desktop->focused_slot,
               (unsigned int) client->role,
               client->active ? 1U : 0U,
               owns_focused ? 1U : 0U);
    }
    for (uint32_t i = 0U; i < WS_KEY_EVENT_BUDGET_PER_TICK; i++)
    {
        int code = sys_kbd_get_scancode();
        if (code <= 0)
            break;
        ws_dbg_key_scancode_read++;
        ws_dbg_last_scancode = (uint32_t) code;

        ws_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = WS_EVENT_KEY;
        event.window_id = desktop->focused_window_id;
        event.key = code;
        if (ws_client_event_push(client, &event))
            ws_dbg_key_push_ok++;
        else
            ws_dbg_key_push_fail++;
    }

    now_tick = sys_tick_get();
    epoch = now_tick / 500ULL;
    if (epoch != 0ULL && epoch > ws_dbg_key_epoch)
    {
        ws_dbg_key_epoch = epoch;
        // #region agent log
        WS_LOG("[AGENTDBG H21 WS_KEYDISPATCH] tick=%llu read=%llu push_ok=%llu push_fail=%llu no_focus=%llu skip_shellgui=%llu skip_shellgui_live=%llu focused_win=%u slot=%u role=%u last_key=%u\n",
               (unsigned long long) now_tick,
               (unsigned long long) ws_dbg_key_scancode_read,
               (unsigned long long) ws_dbg_key_push_ok,
               (unsigned long long) ws_dbg_key_push_fail,
               (unsigned long long) ws_dbg_key_no_focus,
               (unsigned long long) ws_dbg_key_skip_shellgui,
               (unsigned long long) ws_dbg_key_skip_shellgui_live,
               (unsigned int) desktop->focused_window_id,
               (unsigned int) desktop->focused_slot,
               (unsigned int) client->role,
               (unsigned int) ws_dbg_last_scancode);
        // #endregion
    }
}

static void ws_request_close_window(ws_desktop_t* desktop, uint32_t slot, uint32_t window_id)
{
    if (!desktop || slot >= WS_CLIENT_MAX || window_id == 0U)
        return;

    ws_client_conn_t* client = &desktop->clients[slot];
    if (!client->active || !ws_client_owns_window(client, window_id))
        return;

    ws_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = WS_EVENT_CLOSE;
    event.window_id = window_id;
    (void) ws_client_event_push(client, &event);

    if (client->pid > 0U)
        (void) kill((pid_t) client->pid, SIGTERM);

    ws_desktop_invalidate_window_id(desktop, window_id);
    ws_ctx_lock(desktop);
    (void) ws_destroy_window(&desktop->ws, window_id);
    ws_ctx_unlock(desktop);
    ws_client_remove_window(client, window_id);
    if (desktop->focused_window_id == window_id)
        ws_refresh_focus_after_close(desktop);
}

static void ws_begin_drag_if_title(ws_desktop_t* desktop, uint32_t window_id, const ws_window_t* window, int32_t x, int32_t y)
{
    if (!desktop || window_id == 0U || !window)
        return;

    if (!ws_point_in_titlebar(window, x, y))
        return;

    desktop->dragging = true;
    desktop->drag_window_id = window_id;
    desktop->drag_offset_x = x - window->x;
    desktop->drag_offset_y = y - window->y;
}

static void ws_handle_left_press(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop)
        return;

    if (desktop->power_menu_open)
    {
        (void) ws_handle_power_click(desktop, x, y);
        return;
    }

    uint32_t window_id = 0U;
    uint32_t slot = 0U;
    ws_window_t window;
    if (ws_pick_top_client_window_at(desktop, x, y, &window_id, &window, &slot))
    {
        uint32_t top_id = 0U;
        if (desktop->ws.window_count > 0U)
            top_id = desktop->ws.windows[desktop->ws.window_count - 1U].id;
        if (window_id != top_id)
        {
            ws_ctx_lock(desktop);
            int rc = ws_raise_window(&desktop->ws, window_id);
            ws_ctx_unlock(desktop);
            if (rc == 0)
                desktop->compositor_need_full = true;
        }
        ws_set_focus(desktop, window_id, slot);
        WS_LOG("click on client window win=%u slot=%u x=%d y=%d\n",
               (unsigned int) window_id,
               (unsigned int) slot,
               x,
               y);

        if (ws_point_in_close_button(&window, x, y))
        {
            WS_LOG("close requested win=%u slot=%u\n",
                   (unsigned int) window_id,
                   (unsigned int) slot);
            ws_request_close_window(desktop, slot, window_id);
            return;
        }

        ws_begin_drag_if_title(desktop, window_id, &window, x, y);
        return;
    }

    ws_handle_dock_click(desktop, x, y);
    ws_clear_focus(desktop);
}

static bool ws_handle_drag_motion(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop || !desktop->dragging || desktop->drag_window_id == 0U)
        return false;

    ws_window_t window;
    if (!ws_window_fetch(&desktop->ws, desktop->drag_window_id, &window))
    {
        desktop->dragging = false;
        desktop->drag_window_id = 0U;
        return false;
    }

    int32_t nx = x - desktop->drag_offset_x;
    int32_t ny = y - desktop->drag_offset_y;

    int32_t max_x = (int32_t) desktop->ws.mode.hdisplay - (int32_t) ws_min_u32(window.width, (uint32_t) desktop->ws.mode.hdisplay);
    int32_t max_y = (int32_t) desktop->ws.mode.vdisplay - (int32_t) ws_min_u32(window.height, (uint32_t) desktop->ws.mode.vdisplay);
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > max_x)
        nx = max_x;
    if (ny > max_y)
        ny = max_y;

    if (nx == window.x && ny == window.y)
        return false;

    ws_dirty_region_add_rect(desktop, window.x, window.y, window.width, window.height);
    ws_dirty_region_add_rect(desktop, nx, ny, window.width, window.height);
    ws_ctx_lock(desktop);
    (void) ws_move_window(&desktop->ws, desktop->drag_window_id, nx, ny);
    ws_ctx_unlock(desktop);
    return true;
}

static bool ws_process_mouse_events(ws_desktop_t* desktop,
                                    int32_t* cursor_x,
                                    int32_t* cursor_y,
                                    uint8_t* mouse_buttons,
                                    bool* mouse_state_init,
                                    bool* out_cursor_dirty,
                                    bool* out_layout_dirty,
                                    bool* out_layout_full_dirty,
                                    uint32_t budget)
{
    if (!desktop ||
        !cursor_x ||
        !cursor_y ||
        !mouse_buttons ||
        !mouse_state_init ||
        !out_cursor_dirty ||
        !out_layout_dirty ||
        !out_layout_full_dirty ||
        budget == 0U)
        return false;

    *out_cursor_dirty = false;
    *out_layout_dirty = false;
    *out_layout_full_dirty = false;
    bool had_events = false;

    for (uint32_t i = 0U; i < budget; i++)
    {
        syscall_mouse_event_t me;
        int rc = sys_mouse_get_event(&me);
        if (rc <= 0)
            break;

        desktop->stat_mouse_events++;
        had_events = true;

        int32_t sdx = ws_scale_mouse_delta(me.dx);
        int32_t sdy = ws_scale_mouse_delta(me.dy);

        int32_t nx = *cursor_x + sdx;
        int32_t ny = *cursor_y - sdy;
        int32_t max_x = (int32_t) desktop->ws.mode.hdisplay - 1;
        int32_t max_y = (int32_t) desktop->ws.mode.vdisplay - 1;

        if (nx < 0)
            nx = 0;
        if (ny < 0)
            ny = 0;
        if (nx > max_x)
            nx = max_x;
        if (ny > max_y)
            ny = max_y;

        if (nx != *cursor_x || ny != *cursor_y)
        {
            *cursor_x = nx;
            *cursor_y = ny;
            ws_ctx_lock(desktop);
            (void) ws_set_cursor_position(&desktop->ws, *cursor_x, *cursor_y);
            ws_ctx_unlock(desktop);
            *out_cursor_dirty = true;

            if ((me.buttons & WS_MOUSE_BUTTON_LEFT) != 0U)
            {
                if (ws_handle_drag_motion(desktop, *cursor_x, *cursor_y))
                    *out_layout_dirty = true;
            }
        }

        uint8_t buttons = me.buttons;
        if (!*mouse_state_init)
        {
            *mouse_buttons = buttons;
            *mouse_state_init = true;
            continue;
        }

        bool left_pressed = ((buttons & WS_MOUSE_BUTTON_LEFT) != 0U) &&
                              ((*mouse_buttons & WS_MOUSE_BUTTON_LEFT) == 0U);
        bool left_released = ((buttons & WS_MOUSE_BUTTON_LEFT) == 0U) &&
                               ((*mouse_buttons & WS_MOUSE_BUTTON_LEFT) != 0U);

        if (left_pressed)
        {
            desktop->stat_left_presses++;
            ws_handle_left_press(desktop, *cursor_x, *cursor_y);
            *out_layout_dirty = true;
        }

        if (left_released)
        {
            desktop->dragging = false;
            desktop->drag_window_id = 0U;
        }

        *mouse_buttons = buttons;
    }

    return had_events;
}

static bool ws_init_desktop(ws_desktop_t* desktop)
{
    if (!desktop)
        return false;

    memset(desktop, 0, sizeof(*desktop));
    desktop->listen_fd = -1;
    desktop->launcher_pid = -1;
    desktop->focused_window_id = 0U;
    desktop->focused_slot = 0U;
    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        ws_client_reset_slot(&desktop->clients[i]);

    (void) pthread_mutex_init(&desktop->render_mutex, NULL);
    (void) pthread_mutex_init(&desktop->ws_mutex, NULL);

    WS_LOG("[AGENTDBG H35 INIT_STEP] step=1 after_mutex_init\n");
    if (ws_open(&desktop->ws, true) < 0)
    {
        WS_LOG("ws_open failed errno=%d\n", errno);
        return false;
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=2 ws_open_ok mode_raw=%ux%u\n",
           (unsigned int) desktop->ws.mode.hdisplay,
           (unsigned int) desktop->ws.mode.vdisplay);

    uint32_t width = (uint32_t) desktop->ws.mode.hdisplay;
    uint32_t height = (uint32_t) desktop->ws.mode.vdisplay;
    WS_LOG("desktop mode %ux%u\n", (unsigned int) width, (unsigned int) height);
    if (width < 640U || height < 400U)
    {
        WS_LOG("desktop mode too small\n");
        ws_close(&desktop->ws);
        return false;
    }

    if (!ws_tiles_init(desktop))
    {
        WS_LOG("tile init failed\n");
        ws_close(&desktop->ws);
        return false;
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=3 tiles_ok\n");

    (void) ws_set_desktop_color(&desktop->ws, 0x00446682U);
    desktop->power_focus = WS_POWER_ITEM_SHUTDOWN;
    desktop->colors_dirty = true;

    const int32_t margin = 16;
    const uint32_t top_h = 30U;
    const uint32_t dock_h = 88U;
    const uint32_t dock_w = ws_min_u32(520U, width - 64U);
    const int32_t dock_x = ((int32_t) width - (int32_t) dock_w) / 2;
    const int32_t dock_y = (int32_t) height - (int32_t) dock_h - 14;

    ws_window_desc_t top_bar = {
        .x = margin,
        .y = margin,
        .width = width - (uint32_t) (margin * 2),
        .height = top_h,
        .color = 0x003E5F7FU,
        .border_color = 0x00324A62U,
        .titlebar_color = 0x00324A62U,
        .visible = true,
        .frame_controls = false,
        .title = "TheOS WindowServer"
    };

    ws_window_desc_t dock_bar = {
        .x = dock_x,
        .y = dock_y,
        .width = dock_w,
        .height = dock_h,
        .color = 0x00B2C6D8U,
        .border_color = 0x00859DB4U,
        .titlebar_color = 0x00859DB4U,
        .visible = true,
        .frame_controls = false,
        .title = "Launcher"
    };

    if (ws_create_window_checked(&desktop->ws, &top_bar, &desktop->top_bar_id) < 0 ||
        ws_create_window_checked(&desktop->ws, &dock_bar, &desktop->dock_bar_id) < 0)
    {
        WS_LOG("desktop bar create failed errno=%d\n", errno);
        ws_close(&desktop->ws);
        return false;
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=4 bars_ok top=%u dock=%u\n",
           (unsigned int) desktop->top_bar_id,
           (unsigned int) desktop->dock_bar_id);

    const uint32_t icon_w = 120U;
    const uint32_t icon_h = 54U;
    const int32_t icon_gap = 18;
    const int32_t total_w = (int32_t) (icon_w * WS_DOCK_ICON_COUNT) + (int32_t) (icon_gap * (WS_DOCK_ICON_COUNT - 1U));
    const int32_t start_x = dock_x + (((int32_t) dock_w - total_w) / 2);
    const int32_t icon_y = dock_y + 20;

    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        const char* title = "App";
        if (i == WS_DOCK_ITEM_SHELL)
            title = "TheShellGUI";
        else if (i == WS_DOCK_ITEM_SYSTEM_MONITOR)
            title = "TheSystemMonitorGUI";
        else if (i == WS_DOCK_ITEM_POWER)
            title = "Power";

        ws_window_desc_t icon = {
            .x = start_x + (int32_t) (i * icon_w) + (int32_t) (i * icon_gap),
            .y = icon_y,
            .width = icon_w,
            .height = icon_h,
            .color = 0x004C7AA0U,
            .border_color = 0x00385E80U,
            .titlebar_color = 0x00385E80U,
            .visible = true,
            .frame_controls = false,
            .title = title
        };

        if (ws_create_window_checked(&desktop->ws, &icon, &desktop->dock_icon_ids[i]) < 0)
        {
            WS_LOG("dock icon create failed i=%u errno=%d\n", (unsigned int) i, errno);
            ws_close(&desktop->ws);
            return false;
        }
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=5 dock_icons_ok\n");

    const uint32_t panel_w = ws_min_u32(460U, width - 120U);
    const uint32_t panel_h = 214U;
    const int32_t panel_x = ((int32_t) width - (int32_t) panel_w) / 2;
    const int32_t panel_y = ((int32_t) height - (int32_t) panel_h) / 2;

    ws_window_desc_t dimmer = {
        .x = 0,
        .y = 0,
        .width = width,
        .height = height,
        .color = 0x002A3C4DU,
        .border_color = 0x002A3C4DU,
        .titlebar_color = 0x002A3C4DU,
        .visible = false,
        .frame_controls = false,
        .title = ""
    };

    ws_window_desc_t panel = {
        .x = panel_x,
        .y = panel_y,
        .width = panel_w,
        .height = panel_h,
        .color = 0x00D7E1EBU,
        .border_color = 0x007C95AEU,
        .titlebar_color = 0x007C95AEU,
        .visible = false,
        .frame_controls = false,
        .title = "Power"
    };

    if (ws_create_window_checked(&desktop->ws, &dimmer, &desktop->power_dimmer_id) < 0 ||
        ws_create_window_checked(&desktop->ws, &panel, &desktop->power_panel_id) < 0)
    {
        WS_LOG("power overlay create failed errno=%d\n", errno);
        ws_close(&desktop->ws);
        return false;
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=6 power_overlay_ok\n");

    const uint32_t btn_w = (panel_w - 58U) / 3U;
    const uint32_t btn_h = 64U;
    for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
    {
        const char* title = (i == WS_POWER_ITEM_SHUTDOWN) ? "Shutdown" :
                            (i == WS_POWER_ITEM_RESTART) ? "Restart" :
                                                           "Cancel";

        ws_window_desc_t button = {
            .x = panel_x + 18 + (int32_t) (i * (btn_w + 11U)),
            .y = panel_y + (int32_t) panel_h - (int32_t) btn_h - 20,
            .width = btn_w,
            .height = btn_h,
            .color = 0x008BA5BEU,
            .border_color = 0x00627D98U,
            .titlebar_color = 0x00627D98U,
            .visible = false,
            .frame_controls = false,
            .title = title
        };

        if (ws_create_window_checked(&desktop->ws, &button, &desktop->power_button_ids[i]) < 0)
        {
            WS_LOG("power button create failed i=%u errno=%d\n", (unsigned int) i, errno);
            ws_close(&desktop->ws);
            return false;
        }
    }
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=7 power_buttons_ok\n");

    desktop->listen_fd = ws_server_socket_open();
    if (desktop->listen_fd < 0)
    {
        WS_LOG("desktop server socket open failed errno=%d\n", errno);
        ws_close(&desktop->ws);
        return false;
    }

    ws_set_power_menu_visible(desktop, false);
    ws_apply_desktop_colors(desktop);
    ws_dirty_region_reset(desktop);
    WS_LOG("[AGENTDBG H35 INIT_STEP] step=8 server_ready listen_fd=%d\n", desktop->listen_fd);
    WS_LOG("desktop init complete top=%u dock=%u\n",
           (unsigned int) desktop->top_bar_id,
           (unsigned int) desktop->dock_bar_id);
    return true;
}

static void ws_reap_children(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    /* WindowServer only owns the launcher process. */
    if (desktop->launcher_pid <= 0)
        return;

    for (uint32_t i = 0U; i < 2U; i++)
    {
        int status = 0;
        pid_t pid = waitpid(desktop->launcher_pid, &status, WNOHANG);
        if (pid <= 0)
            return;

        WS_LOG("launcher exited pid=%d status=0x%X\n", (int) pid, (unsigned int) status);
        desktop->launcher_pid = -1;
        return;
    }
}

__attribute__((unused)) static bool ws_render_cursor_delta(ws_desktop_t* desktop,
                                   int32_t prev_cursor_x,
                                   int32_t prev_cursor_y,
                                   int32_t cursor_x,
                                   int32_t cursor_y)
{
    if (!desktop)
        return false;

    int32_t max_w = (int32_t) desktop->ws.mode.hdisplay;
    int32_t max_h = (int32_t) desktop->ws.mode.vdisplay;
    if (max_w <= 0 || max_h <= 0)
        return false;

    int32_t box_w = (int32_t) WS_CURSOR_WIDTH + (int32_t) WS_CURSOR_SHADOW_OFFSET;
    int32_t box_h = (int32_t) WS_CURSOR_HEIGHT + (int32_t) WS_CURSOR_SHADOW_OFFSET;

    int32_t x0 = prev_cursor_x;
    int32_t y0 = prev_cursor_y;
    int32_t x1 = prev_cursor_x + box_w;
    int32_t y1 = prev_cursor_y + box_h;

    int32_t nx0 = cursor_x;
    int32_t ny0 = cursor_y;
    int32_t nx1 = cursor_x + box_w;
    int32_t ny1 = cursor_y + box_h;

    if (nx0 < x0)
        x0 = nx0;
    if (ny0 < y0)
        y0 = ny0;
    if (nx1 > x1)
        x1 = nx1;
    if (ny1 > y1)
        y1 = ny1;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > max_w)
        x1 = max_w;
    if (y1 > max_h)
        y1 = max_h;
    if (x0 >= x1 || y0 >= y1)
        return false;

    return ws_render_region(&desktop->ws,
                            x0,
                            y0,
                            (uint32_t) (x1 - x0),
                            (uint32_t) (y1 - y0)) == 0;
}

int main(void)
{
    WS_LOG("[AGENTDBG H35 MAIN_STEP] step=1 main_enter\n");
    pid_t launcher_pid = ws_launcher_start();
    WS_LOG("[AGENTDBG H35 MAIN_STEP] step=2 launcher_pid=%d errno=%d\n", (int) launcher_pid, errno);
    if (launcher_pid <= 0)
    {
        WS_LOG("startup failed: launcher not started errno=%d\n", errno);
        return 1;
    }

    WS_LOG("[AGENTDBG H35 MAIN_STEP] step=3 before_init_desktop\n");
    if (!ws_init_desktop(&desktop))
    {
        (void) kill(launcher_pid, SIGTERM);
        (void) waitpid(launcher_pid, NULL, WNOHANG);
        WS_LOG("startup failed: desktop init failed\n");
        return 1;
    }
    desktop.launcher_pid = launcher_pid;
    WS_LOG("startup complete launcher_pid=%d\n", (int) desktop.launcher_pid);
    WS_LOG("framebuffer map=0x%llx pitch=%u size=%llu mode=%ux%u\n",
           (unsigned long long) (uintptr_t) desktop.ws.frame_map,
           (unsigned int) desktop.ws.dumb_pitch,
           (unsigned long long) desktop.ws.dumb_size,
           (unsigned int) desktop.ws.mode.hdisplay,
           (unsigned int) desktop.ws.mode.vdisplay);

    if (pthread_create(&desktop.render_thread, NULL, ws_render_thread_main, &desktop) == 0)
    {
        desktop.render_thread_running = true;
    }
    else
    {
        WS_LOG("render thread start failed\n");
    }

    (void) pthread_mutex_lock(&desktop.render_mutex);
    ws_tiles_mark_all(&desktop);
    desktop.render_full_pending = true;
    (void) pthread_mutex_unlock(&desktop.render_mutex);

    int32_t cursor_x = (int32_t) desktop.ws.mode.hdisplay / 2;
    int32_t cursor_y = (int32_t) desktop.ws.mode.vdisplay / 2;
    uint8_t mouse_buttons = 0U;
    bool mouse_state_init = false;
    int32_t rendered_cursor_x = cursor_x;
    int32_t rendered_cursor_y = cursor_y;
    bool layout_dirty = true;
    bool layout_full_dirty = true;
    bool cursor_dirty = false;
    uint64_t next_render_tick = 0ULL;
    uint64_t first_render_tick = sys_tick_get() + WS_FIRST_RENDER_DELAY_TICKS;
    uint64_t next_heartbeat_tick = sys_tick_get() + WS_HEARTBEAT_TICKS;
#if WS_PROFILE_ENABLED
    uint64_t next_profile_tick = sys_tick_get() + WS_PROFILE_LOG_TICKS;
    uint64_t prev_stat_requests = 0ULL;
    uint64_t prev_stat_poll_event = 0ULL;
    uint64_t prev_stat_poll_eagain = 0ULL;
    uint64_t prev_stat_set_text = 0ULL;
    uint64_t prev_stat_set_text_noop = 0ULL;
    uint64_t prev_stat_renders = 0ULL;
    uint64_t prev_stat_partial_renders = 0ULL;
    uint64_t prev_stat_render_ticks = 0ULL;
#endif
    ws_loop_profile_t profile;
    memset(&profile, 0, sizeof(profile));

    ws_ctx_lock(&desktop);
    (void) ws_set_cursor_visible(&desktop.ws, true);
    (void) ws_set_cursor_color(&desktop.ws, 0x00F4F7FCU);
    (void) ws_set_cursor_position(&desktop.ws, cursor_x, cursor_y);
    ws_ctx_unlock(&desktop);

    for (;;)
    {
        uint64_t loop_start_tick = sys_tick_get();
        profile.loops++;
        bool had_activity = false;
        int32_t cursor_before_loop_x = cursor_x;
        int32_t cursor_before_loop_y = cursor_y;

        ws_reap_children(&desktop);

        /* Mouse before accept/IPC so cursor stays responsive under GUI load. */
        bool mouse_cursor_dirty = false;
        bool mouse_layout_dirty = false;
        bool mouse_layout_full_dirty = false;
        uint64_t stage_start_tick = sys_tick_get();
        if (ws_process_mouse_events(&desktop,
                                    &cursor_x,
                                    &cursor_y,
                                    &mouse_buttons,
                                    &mouse_state_init,
                                    &mouse_cursor_dirty,
                                    &mouse_layout_dirty,
                                    &mouse_layout_full_dirty,
                                    WS_MOUSE_EVENT_BUDGET_PER_TICK))
        {
            had_activity = true;
        }
        ws_profile_stage_record(&profile.mouse_pre_calls,
                                &profile.mouse_pre_ticks_total,
                                &profile.mouse_pre_ticks_max,
                                &profile.mouse_pre_slow,
                                ws_tick_delta(sys_tick_get(), stage_start_tick));
        if (mouse_cursor_dirty)
            cursor_dirty = true;
        if (mouse_layout_dirty)
            layout_dirty = true;
        if (mouse_layout_full_dirty)
            layout_full_dirty = true;

        uint64_t accepts_before = desktop.stat_accepts;
        stage_start_tick = sys_tick_get();
        if (ws_accept_clients(&desktop))
        {
            layout_dirty = true;
            had_activity = true;
        }
        ws_profile_stage_record(&profile.accept_calls,
                                &profile.accept_ticks_total,
                                &profile.accept_ticks_max,
                                &profile.accept_slow,
                                ws_tick_delta(sys_tick_get(), stage_start_tick));
        if (desktop.stat_accepts != accepts_before)
            had_activity = true;

        stage_start_tick = sys_tick_get();
        bool poll_activity = false;
        if (ws_poll_clients(&desktop, &poll_activity))
        {
            layout_dirty = true;
            had_activity = true;
        }
        ws_profile_stage_record(&profile.poll_calls,
                                &profile.poll_ticks_total,
                                &profile.poll_ticks_max,
                                &profile.poll_slow,
                                ws_tick_delta(sys_tick_get(), stage_start_tick));
        if (poll_activity)
            had_activity = true;

        /* Drain again after IPC: chatty clients (e.g. ShellGUI text capture) must not starve PS/2 queue. */
        mouse_cursor_dirty = false;
        mouse_layout_dirty = false;
        mouse_layout_full_dirty = false;
        stage_start_tick = sys_tick_get();
        if (ws_process_mouse_events(&desktop,
                                    &cursor_x,
                                    &cursor_y,
                                    &mouse_buttons,
                                    &mouse_state_init,
                                    &mouse_cursor_dirty,
                                    &mouse_layout_dirty,
                                    &mouse_layout_full_dirty,
                                    WS_MOUSE_EVENT_BUDGET_AFTER_CLIENTS))
        {
            had_activity = true;
        }
        ws_profile_stage_record(&profile.mouse_post_calls,
                                &profile.mouse_post_ticks_total,
                                &profile.mouse_post_ticks_max,
                                &profile.mouse_post_slow,
                                ws_tick_delta(sys_tick_get(), stage_start_tick));
        if (mouse_cursor_dirty)
            cursor_dirty = true;
        if (mouse_layout_dirty)
            layout_dirty = true;
        if (mouse_layout_full_dirty)
            layout_full_dirty = true;

        ws_dispatch_key_events(&desktop);

        if (desktop.compositor_need_full)
        {
            layout_full_dirty = true;
            desktop.compositor_need_full = false;
        }

        if (desktop.colors_dirty)
        {
            ws_ctx_lock(&desktop);
            ws_apply_desktop_colors(&desktop);
            ws_ctx_unlock(&desktop);
            ws_desktop_invalidate_chrome(&desktop);
            desktop.colors_dirty = false;
            layout_dirty = true;
        }

        if (layout_dirty || cursor_dirty)
        {
            uint64_t now_tick = sys_tick_get();
            if (now_tick < first_render_tick)
            {
                /* Delay first render briefly without clearing dirty flags. */
            }
            else
            {
                if (cursor_dirty)
                    ws_dirty_region_add_cursor_delta(&desktop,
                                                     rendered_cursor_x,
                                                     rendered_cursor_y,
                                                     cursor_x,
                                                     cursor_y);

                if (layout_full_dirty)
                {
                    (void) pthread_mutex_lock(&desktop.render_mutex);
                    ws_tiles_clear(&desktop);
                    ws_tiles_mark_all(&desktop);
                    desktop.render_full_pending = true;
                    (void) pthread_mutex_unlock(&desktop.render_mutex);
                }
                else
                {
                    int32_t rx = 0;
                    int32_t ry = 0;
                    uint32_t rw = 0U;
                    uint32_t rh = 0U;
                    if (ws_dirty_region_take(&desktop, &rx, &ry, &rw, &rh))
                    {
                        (void) pthread_mutex_lock(&desktop.render_mutex);
                        ws_tiles_mark_rect(&desktop, rx, ry, rw, rh);
                        (void) pthread_mutex_unlock(&desktop.render_mutex);
                    }
                }

                rendered_cursor_x = cursor_x;
                rendered_cursor_y = cursor_y;
                layout_dirty = false;
                layout_full_dirty = false;
                cursor_dirty = false;
                next_render_tick = now_tick + WS_RENDER_MIN_INTERVAL_TICKS;
                had_activity = true;
            }
        }
        else if (cursor_before_loop_x != cursor_x || cursor_before_loop_y != cursor_y)
        {
            cursor_dirty = true;
        }

        if (had_activity)
        {
            if (WS_ACTIVE_SLEEP_US != 0U)
                (void) usleep(WS_ACTIVE_SLEEP_US);
        }
        else
        {
            if (WS_IDLE_SLEEP_US == 0U)
                (void) sys_yield();
            else
                (void) usleep(WS_IDLE_SLEEP_US);
        }
        if (had_activity)
            profile.loops_activity++;
        uint64_t loop_ticks = ws_tick_delta(sys_tick_get(), loop_start_tick);
        profile.loop_ticks_total += loop_ticks;
        if (loop_ticks > profile.loop_ticks_max)
            profile.loop_ticks_max = loop_ticks;

        uint64_t now_tick = sys_tick_get();
        if (now_tick >= next_heartbeat_tick)
        {
#if WS_HEARTBEAT_VERBOSE
            uint64_t render_count = desktop.stat_renders;
            uint64_t avg_render_ticks = (render_count != 0ULL) ?
                                            (desktop.stat_render_ticks_total / render_count) :
                                            0ULL;
            WS_LOG("hb core tick=%llu clients=%u client_windows=%u desktop_windows=%u accepts=%llu accept_err=%llu req=%llu create_ok=%llu create_fail=%llu recv_fail=%llu send_fail=%llu mouse_events=%llu clicks=%llu\n",
                   (unsigned long long) now_tick,
                   (unsigned int) ws_active_client_count(&desktop),
                   (unsigned int) ws_active_client_windows(&desktop),
                   (unsigned int) desktop.ws.window_count,
                   (unsigned long long) desktop.stat_accepts,
                   (unsigned long long) desktop.stat_accept_errors,
                   (unsigned long long) desktop.stat_requests,
                   (unsigned long long) desktop.stat_req_create_ok,
                   (unsigned long long) desktop.stat_req_create_fail,
                   (unsigned long long) desktop.stat_recv_fail,
                   (unsigned long long) desktop.stat_send_fail,
                   (unsigned long long) desktop.stat_mouse_events,
                   (unsigned long long) desktop.stat_left_presses);

            WS_LOG("hb render renders=%llu partial=%llu render_fail=%llu avg_ticks=%llu max_ticks=%llu slow=%llu throttle_skip=%llu launcher_pid=%d launcher_sent=%llu launcher_send_fail=%llu\n",
                   (unsigned long long) desktop.stat_renders,
                   (unsigned long long) desktop.stat_partial_renders,
                   (unsigned long long) desktop.stat_render_fail,
                   (unsigned long long) avg_render_ticks,
                   (unsigned long long) desktop.stat_render_ticks_max,
                   (unsigned long long) desktop.stat_render_slow,
                   (unsigned long long) desktop.stat_render_throttle_skip,
                   (int) desktop.launcher_pid,
                   (unsigned long long) desktop.stat_launcher_cmd_sent,
                   (unsigned long long) desktop.stat_launcher_cmd_send_fail);

            uint64_t loop_avg_ticks = (profile.loops != 0ULL) ?
                                          (profile.loop_ticks_total / profile.loops) :
                                          0ULL;
            uint64_t mouse_pre_avg_ticks = (profile.mouse_pre_calls != 0ULL) ?
                                               (profile.mouse_pre_ticks_total / profile.mouse_pre_calls) :
                                               0ULL;
            uint64_t accept_avg_ticks = (profile.accept_calls != 0ULL) ?
                                            (profile.accept_ticks_total / profile.accept_calls) :
                                            0ULL;
            uint64_t poll_avg_ticks = (profile.poll_calls != 0ULL) ?
                                          (profile.poll_ticks_total / profile.poll_calls) :
                                          0ULL;
            uint64_t mouse_post_avg_ticks = (profile.mouse_post_calls != 0ULL) ?
                                                (profile.mouse_post_ticks_total / profile.mouse_post_calls) :
                                                0ULL;
            uint64_t render_full_avg_ticks = (profile.render_full_count != 0ULL) ?
                                                 (profile.render_full_ticks_total / profile.render_full_count) :
                                                 0ULL;
            uint64_t render_region_avg_ticks = (profile.render_region_count != 0ULL) ?
                                                   (profile.render_region_ticks_total / profile.render_region_count) :
                                                   0ULL;
            uint64_t render_cursor_avg_ticks = (profile.render_cursor_count != 0ULL) ?
                                                   (profile.render_cursor_ticks_total / profile.render_cursor_count) :
                                                   0ULL;

            WS_LOG("hb perf loop loops=%llu active=%llu avg_ticks=%llu max_ticks=%llu\n",
                   (unsigned long long) profile.loops,
                   (unsigned long long) profile.loops_activity,
                   (unsigned long long) loop_avg_ticks,
                   (unsigned long long) profile.loop_ticks_max);
            WS_LOG("hb perf stage mouse_pre avg=%llu max=%llu slow=%llu accept avg=%llu max=%llu slow=%llu poll avg=%llu max=%llu slow=%llu mouse_post avg=%llu max=%llu slow=%llu\n",
                   (unsigned long long) mouse_pre_avg_ticks,
                   (unsigned long long) profile.mouse_pre_ticks_max,
                   (unsigned long long) profile.mouse_pre_slow,
                   (unsigned long long) accept_avg_ticks,
                   (unsigned long long) profile.accept_ticks_max,
                   (unsigned long long) profile.accept_slow,
                   (unsigned long long) poll_avg_ticks,
                   (unsigned long long) profile.poll_ticks_max,
                   (unsigned long long) profile.poll_slow,
                   (unsigned long long) mouse_post_avg_ticks,
                   (unsigned long long) profile.mouse_post_ticks_max,
                   (unsigned long long) profile.mouse_post_slow);
            WS_LOG("hb perf render full=%llu avg=%llu max=%llu region=%llu avg=%llu max=%llu px=%llu cursor=%llu avg=%llu max=%llu\n",
                   (unsigned long long) profile.render_full_count,
                   (unsigned long long) render_full_avg_ticks,
                   (unsigned long long) profile.render_full_ticks_max,
                   (unsigned long long) profile.render_region_count,
                   (unsigned long long) render_region_avg_ticks,
                   (unsigned long long) profile.render_region_ticks_max,
                   (unsigned long long) profile.render_region_pixels_total,
                   (unsigned long long) profile.render_cursor_count,
                   (unsigned long long) render_cursor_avg_ticks,
                   (unsigned long long) profile.render_cursor_ticks_max);

            syscall_mouse_debug_info_t mouse_dbg;
            if (sys_mouse_debug_info_get(&mouse_dbg) == 0)
            {
                WS_LOG("hb mouse irq12_cb=%llu irq12_bytes=%llu irq12_aux=%llu irq12_nonaux=%llu irq12_budget_hits=%llu irq1_aux=%llu\n",
                       (unsigned long long) mouse_dbg.irq12_callbacks,
                       (unsigned long long) mouse_dbg.irq12_bytes_total,
                       (unsigned long long) mouse_dbg.irq12_aux_bytes,
                       (unsigned long long) mouse_dbg.irq12_non_aux_bytes,
                       (unsigned long long) mouse_dbg.irq12_drain_budget_hits,
                       (unsigned long long) mouse_dbg.irq1_aux_bytes);
                WS_LOG("hb mouse poll_cyc=%llu poll_bytes=%llu poll_aux=%llu poll_nonaux=%llu pushed=%llu popped=%llu empty=%llu drop_full=%llu sync_drop=%llu ovf_drop=%llu q=%u wp=%u rp=%u pkt=%u ready=%u\n",
                       (unsigned long long) mouse_dbg.poll_cycles,
                       (unsigned long long) mouse_dbg.poll_bytes_total,
                       (unsigned long long) mouse_dbg.poll_aux_bytes,
                       (unsigned long long) mouse_dbg.poll_non_aux_bytes,
                       (unsigned long long) mouse_dbg.events_pushed,
                       (unsigned long long) mouse_dbg.events_popped,
                       (unsigned long long) mouse_dbg.get_event_empty,
                       (unsigned long long) mouse_dbg.events_dropped_full,
                       (unsigned long long) mouse_dbg.packet_sync_drops,
                       (unsigned long long) mouse_dbg.packet_overflow_drops,
                       (unsigned int) mouse_dbg.queue_count,
                       (unsigned int) mouse_dbg.queue_write_pos,
                       (unsigned int) mouse_dbg.queue_read_pos,
                       (unsigned int) mouse_dbg.packet_index,
                       (unsigned int) mouse_dbg.ready);
                WS_LOG("hb mouse forced_req attempts=%llu ok=%llu fail=%llu\n",
                       (unsigned long long) mouse_dbg.forced_request_attempts,
                       (unsigned long long) mouse_dbg.forced_request_success,
                       (unsigned long long) mouse_dbg.forced_request_fail);
            }
            else
            {
                WS_LOG("hb mouse debug syscall failed errno=%d\n", errno);
            }
#else
            {
                uint64_t render_count = desktop.stat_renders;
                uint64_t avg_render_ticks = (render_count != 0ULL) ?
                                                (desktop.stat_render_ticks_total / render_count) :
                                                0ULL;
                WS_LOG("hb tick=%llu cli=%u cw=%u dw=%u req=%llu rnd=%llu prt=%llu fail=%llu avgt=%llu maxt=%llu mev=%llu\n",
                       (unsigned long long) now_tick,
                       (unsigned int) ws_active_client_count(&desktop),
                       (unsigned int) ws_active_client_windows(&desktop),
                       (unsigned int) desktop.ws.window_count,
                       (unsigned long long) desktop.stat_requests,
                       (unsigned long long) desktop.stat_renders,
                       (unsigned long long) desktop.stat_partial_renders,
                       (unsigned long long) desktop.stat_render_fail,
                       (unsigned long long) avg_render_ticks,
                       (unsigned long long) desktop.stat_render_ticks_max,
                       (unsigned long long) desktop.stat_mouse_events);
            }
#endif
            memset(&profile, 0, sizeof(profile));
            next_heartbeat_tick = now_tick + WS_HEARTBEAT_TICKS;
        }

#if WS_PROFILE_ENABLED
        if (now_tick >= next_profile_tick)
        {
            uint64_t renders = desktop.stat_renders;
            uint64_t render_ticks = desktop.stat_render_ticks_total;
            uint64_t render_delta = renders - prev_stat_renders;
            uint64_t render_ticks_delta = render_ticks - prev_stat_render_ticks;
            uint64_t avg_render = (render_delta != 0ULL) ? (render_ticks_delta / render_delta) : 0ULL;

            uint64_t poll_event_delta = desktop.stat_poll_event - prev_stat_poll_event;
            uint64_t poll_eagain_delta = desktop.stat_poll_eagain - prev_stat_poll_eagain;
            uint64_t set_text_delta = desktop.stat_set_text - prev_stat_set_text;
            uint64_t set_text_noop_delta = desktop.stat_set_text_noop - prev_stat_set_text_noop;
            uint64_t req_delta = desktop.stat_requests - prev_stat_requests;

            uint64_t loop_avg_ticks = (profile.loops != 0ULL) ?
                                          (profile.loop_ticks_total / profile.loops) :
                                          0ULL;
            uint64_t mouse_pre_avg_ticks = (profile.mouse_pre_calls != 0ULL) ?
                                               (profile.mouse_pre_ticks_total / profile.mouse_pre_calls) :
                                               0ULL;
            uint64_t accept_avg_ticks = (profile.accept_calls != 0ULL) ?
                                            (profile.accept_ticks_total / profile.accept_calls) :
                                            0ULL;
            uint64_t poll_avg_ticks = (profile.poll_calls != 0ULL) ?
                                          (profile.poll_ticks_total / profile.poll_calls) :
                                          0ULL;
            uint64_t mouse_post_avg_ticks = (profile.mouse_post_calls != 0ULL) ?
                                                (profile.mouse_post_ticks_total / profile.mouse_post_calls) :
                                                0ULL;
            uint64_t partial_delta = desktop.stat_partial_renders - prev_stat_partial_renders;

            WS_LOG("prof tick=%llu loops=%llu loop_avg=%llu max=%llu activity=%llu\n",
                   (unsigned long long) now_tick,
                   (unsigned long long) profile.loops,
                   (unsigned long long) loop_avg_ticks,
                   (unsigned long long) profile.loop_ticks_max,
                   (unsigned long long) profile.loops_activity);
            WS_LOG("prof ipc poll=%llu eagain=%llu set_text=%llu noop=%llu req=%llu\n",
                   (unsigned long long) poll_event_delta,
                   (unsigned long long) poll_eagain_delta,
                   (unsigned long long) set_text_delta,
                   (unsigned long long) set_text_noop_delta,
                   (unsigned long long) req_delta);
            WS_LOG("prof stage mouse_pre avg=%llu max=%llu accept avg=%llu max=%llu poll avg=%llu max=%llu mouse_post avg=%llu max=%llu\n",
                   (unsigned long long) mouse_pre_avg_ticks,
                   (unsigned long long) profile.mouse_pre_ticks_max,
                   (unsigned long long) accept_avg_ticks,
                   (unsigned long long) profile.accept_ticks_max,
                   (unsigned long long) poll_avg_ticks,
                   (unsigned long long) profile.poll_ticks_max,
                   (unsigned long long) mouse_post_avg_ticks,
                   (unsigned long long) profile.mouse_post_ticks_max);
            WS_LOG("prof render renders=%llu partial=%llu avg=%llu max=%llu\n",
                   (unsigned long long) render_delta,
                   (unsigned long long) partial_delta,
                   (unsigned long long) avg_render,
                   (unsigned long long) desktop.stat_render_ticks_max);

            prev_stat_poll_event = desktop.stat_poll_event;
            prev_stat_poll_eagain = desktop.stat_poll_eagain;
            prev_stat_set_text = desktop.stat_set_text;
            prev_stat_set_text_noop = desktop.stat_set_text_noop;
            prev_stat_requests = desktop.stat_requests;
            prev_stat_renders = renders;
            prev_stat_partial_renders = desktop.stat_partial_renders;
            prev_stat_render_ticks = render_ticks;
            next_profile_tick = now_tick + WS_PROFILE_LOG_TICKS;
            memset(&profile, 0, sizeof(profile));
        }
#endif
    }

    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        ws_client_disconnect_slot(&desktop, i);
    if (desktop.listen_fd >= 0)
        (void) close(desktop.listen_fd);
    if (desktop.launcher_pid > 0)
    {
        (void) kill(desktop.launcher_pid, SIGTERM);
        (void) waitpid(desktop.launcher_pid, NULL, WNOHANG);
    }
    ws_close(&desktop.ws);
    WS_LOG("shutdown\n");
    return 1;
}
