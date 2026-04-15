#ifndef WS_CONFIG_H
#define WS_CONFIG_H

#include <stdint.h>
#include <stdio.h>
#include <syscall.h>

#define WS_TITLEBAR_HEIGHT      20U
#define WS_CLOSE_BUTTON_SIZE    14U
#define WS_CLOSE_BUTTON_MARGINX 4U
#define WS_CLOSE_BUTTON_MARGINY 3U
#define WS_WINDOW_BODY_PAD_X    6U
#define WS_WINDOW_BODY_PAD_Y    4U
#define WS_WINDOW_BODY_BOTTOM_MARGIN 3U
#define WS_RENDER_TILE_W 128U
#define WS_RENDER_TILE_H 128U
#define WS_RENDER_TIME_BUDGET_TICKS 2ULL
#define WS_SERVER_BACKLOG       16
#define WS_CLIENT_MAX           16U
#define WS_CLIENT_EVENT_QUEUE   32U
#define WS_IPC_SEND_CHUNK_BYTES 1024U
#define WS_IPC_SEND_RETRY_BUDGET 8U
#define WS_ACCEPT_BUDGET_PER_TICK 16U
#define WS_GUI_SPAWN_HANDSHAKE_GRACE_TICKS 3000ULL
#define WS_CLIENT_REQ_BUDGET_PER_TICK 56U
#define WS_CLIENT_IO_ITER_BUDGET 160U
#define WS_CLIENT_RECV_SOFT_ERR_BUDGET 8U
#define WS_MOUSE_EVENT_BUDGET_PER_TICK 512U
#define WS_MOUSE_EVENT_BUDGET_AFTER_CLIENTS 256U
#define WS_KEY_EVENT_BUDGET_PER_TICK 64U
#define WS_MOUSE_ACCEL_SMALL_MULT 1
#define WS_MOUSE_ACCEL_MEDIUM_MULT 1
#define WS_MOUSE_DELTA_EVENT_CLAMP 24
#define WS_ACTIVE_YIELD_EVERY_LOOPS 16U
#define WS_RENDER_SLOW_TICKS 32ULL
#define WS_FIRST_RENDER_DELAY_TICKS 1ULL
#define WS_HEARTBEAT_TICKS 4000ULL
#define WS_STAGE_SLOW_TICKS     2ULL
#define WS_MOUSE_BUTTON_LEFT 0x01U
#define WS_DOCK_ICON_COUNT  3U
#define WS_POWER_ITEM_COUNT 3U
#define WS_POWERMANAGER_STALE_TICKS 500ULL

#ifndef WS_HEARTBEAT_VERBOSE
#define WS_HEARTBEAT_VERBOSE 0
#endif
#ifndef WS_IPC_TRACE
#define WS_IPC_TRACE 0
#endif
#ifndef WS_PROFILE_ENABLED
#define WS_PROFILE_ENABLED 1
#endif
#ifndef WS_PROFILE_LOG_TICKS
#define WS_PROFILE_LOG_TICKS 2000ULL
#endif

#define WS_LOG(fmt, ...)                                                       \
    do                                                                         \
    {                                                                          \
        unsigned long long __tick = (unsigned long long) sys_tick_get();        \
        printf("[TheWindowServer t=%llu] " fmt, __tick, ##__VA_ARGS__);        \
        (void) fflush(stdout);                                                 \
    }                                                                          \
    while (0)

static inline int32_t ws_abs_i32(int32_t value)
{
    return (value < 0) ? -value : value;
}

static inline uint32_t ws_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static inline uint64_t ws_tick_delta(uint64_t end_tick, uint64_t start_tick)
{
    return (end_tick >= start_tick) ? (end_tick - start_tick) : 0ULL;
}

static inline void ws_copy_cstr_bounded(char* out, size_t out_size, const char* in)
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
    __builtin_memcpy(out, in, len);
    out[len] = '\0';
}

static inline void ws_profile_stage_record(uint64_t* calls,
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

#endif
