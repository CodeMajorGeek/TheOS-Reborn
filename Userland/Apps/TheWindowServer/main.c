#include "Includes/ws_types.h"
#include "Includes/ws_compositor.h"
#include "Includes/ws_ipc_server.h"
#include "Includes/ws_input.h"
#include "Includes/ws_focus.h"
#include "Includes/ws_desktop_ui.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

static ws_desktop_t desktop;

int main(void)
{
    if (!ws_init_desktop(&desktop))
    {
        WS_LOG("startup failed: desktop init failed\n");
        return 1;
    }

    desktop.render_ctx_snapshot = &desktop.ws;

    WS_LOG("startup complete\n");
    WS_LOG("framebuffer map=0x%llx pitch=%u size=%llu mode=%ux%u\n",
           (unsigned long long) (uintptr_t) desktop.ws.frame_map,
           (unsigned int) desktop.ws.dumb_pitch,
           (unsigned long long) desktop.ws.dumb_size,
           (unsigned int) desktop.ws.mode.hdisplay,
           (unsigned int) desktop.ws.mode.vdisplay);

    {
        int ws_owner = sys_thread_self();
        if (ws_owner > 0)
        {
            if (sys_kbd_capture_set((uint32_t) ws_owner) < 0)
                WS_LOG("kbd capture set failed errno=%d\n", errno);
            else
                WS_LOG("kbd hardware capture owner=%d\n", ws_owner);
        }
    }

    if (pthread_create(&desktop.render_thread, NULL, ws_render_thread_main, &desktop) == 0)
        desktop.render_thread_running = true;
    else
    {
        WS_LOG("render thread start failed\n");
        desktop.render_ctx_snapshot = NULL;
    }

    if (ws_launch_gui_app("/bin/TheShellGUI") < 0)
        WS_LOG("autostart TheShellGUI launch failed errno=%d\n", errno);
    else
        WS_LOG("autostart TheShellGUI launched\n");

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
    uint64_t first_render_tick = sys_tick_get() + WS_FIRST_RENDER_DELAY_TICKS;
    uint64_t next_heartbeat_tick = sys_tick_get() + WS_HEARTBEAT_TICKS;
    uint64_t hb_prev_set_text = 0ULL;
    uint64_t hb_prev_set_text_noop = 0ULL;
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
    uint32_t ws_active_loops_since_yield = 0U;

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

        if (ws_accept_clients(&desktop))
        {
            layout_dirty = true;
            had_activity = true;
        }

        /* Mouse before IPC for cursor responsiveness */
        bool mouse_cursor_dirty = false;
        bool mouse_layout_dirty = false;
        bool mouse_layout_full_dirty = false;
        uint64_t stage_start_tick = sys_tick_get();
        if (ws_process_mouse_events(&desktop, &cursor_x, &cursor_y,
                                    &mouse_buttons, &mouse_state_init,
                                    &mouse_cursor_dirty, &mouse_layout_dirty,
                                    &mouse_layout_full_dirty,
                                    WS_MOUSE_EVENT_BUDGET_PER_TICK))
            had_activity = true;
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

        if (ws_try_flush_cursor_tiles_early(&desktop, cursor_x, cursor_y,
                                            &rendered_cursor_x, &rendered_cursor_y,
                                            &cursor_dirty, layout_dirty,
                                            layout_full_dirty, sys_tick_get(),
                                            first_render_tick))
            had_activity = true;

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

        /* Post-IPC mouse drain */
        mouse_cursor_dirty = false;
        mouse_layout_dirty = false;
        mouse_layout_full_dirty = false;
        stage_start_tick = sys_tick_get();
        if (ws_process_mouse_events(&desktop, &cursor_x, &cursor_y,
                                    &mouse_buttons, &mouse_state_init,
                                    &mouse_cursor_dirty, &mouse_layout_dirty,
                                    &mouse_layout_full_dirty,
                                    WS_MOUSE_EVENT_BUDGET_AFTER_CLIENTS))
            had_activity = true;
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

        if (ws_try_flush_cursor_tiles_early(&desktop, cursor_x, cursor_y,
                                            &rendered_cursor_x, &rendered_cursor_y,
                                            &cursor_dirty, layout_dirty,
                                            layout_full_dirty, sys_tick_get(),
                                            first_render_tick))
            had_activity = true;

        ws_dispatch_key_events(&desktop);

        if (desktop.compositor_need_full)
        {
            layout_full_dirty = true;
            layout_dirty = true;
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
            if (now_tick >= first_render_tick)
            {
                if (cursor_dirty)
                {
                    ws_mark_cursor_move_tiles(&desktop, rendered_cursor_x,
                                              rendered_cursor_y, cursor_x, cursor_y);
                    rendered_cursor_x = cursor_x;
                    rendered_cursor_y = cursor_y;
                    cursor_dirty = false;
                }

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
                    int32_t rx = 0, ry = 0;
                    uint32_t rw = 0U, rh = 0U;
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
                had_activity = true;
            }
        }
        else if (cursor_before_loop_x != cursor_x || cursor_before_loop_y != cursor_y)
            cursor_dirty = true;

        if (had_activity)
        {
            ws_active_loops_since_yield++;
            if (ws_active_loops_since_yield >= WS_ACTIVE_YIELD_EVERY_LOOPS)
            {
                (void) sys_yield();
                ws_active_loops_since_yield = 0U;
            }
        }
        else
            (void) sys_yield();

        if (had_activity)
            profile.loops_activity++;
        uint64_t loop_ticks = ws_tick_delta(sys_tick_get(), loop_start_tick);
        profile.loop_ticks_total += loop_ticks;
        if (loop_ticks > profile.loop_ticks_max)
            profile.loop_ticks_max = loop_ticks;

        uint64_t now_tick = sys_tick_get();
        if (now_tick >= next_heartbeat_tick)
        {
            uint64_t render_count = desktop.stat_renders;
            uint64_t avg_render_ticks = (render_count != 0ULL) ?
                                            (desktop.stat_render_ticks_total / render_count) :
                                            0ULL;
            uint64_t stx_d = desktop.stat_set_text - hb_prev_set_text;
            uint64_t stn_d = desktop.stat_set_text_noop - hb_prev_set_text_noop;
            hb_prev_set_text = desktop.stat_set_text;
            hb_prev_set_text_noop = desktop.stat_set_text_noop;
            WS_LOG("hb tick=%llu cli=%u cw=%u dw=%u req=%llu rnd=%llu prt=%llu fail=%llu avgt=%llu maxt=%llu mev=%llu stx=%llu stn=%llu\n",
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
                   (unsigned long long) desktop.stat_mouse_events,
                   (unsigned long long) stx_d,
                   (unsigned long long) stn_d);
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
            uint64_t partial_delta = desktop.stat_partial_renders - prev_stat_partial_renders;

            WS_LOG("prof tick=%llu loops=%llu req=%llu poll=%llu eagain=%llu stx=%llu noop=%llu renders=%llu partial=%llu avg=%llu\n",
                   (unsigned long long) now_tick,
                   (unsigned long long) profile.loops,
                   (unsigned long long) req_delta,
                   (unsigned long long) poll_event_delta,
                   (unsigned long long) poll_eagain_delta,
                   (unsigned long long) set_text_delta,
                   (unsigned long long) set_text_noop_delta,
                   (unsigned long long) render_delta,
                   (unsigned long long) partial_delta,
                   (unsigned long long) avg_render);

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
    if (desktop.render_thread_running)
    {
        desktop.render_thread_exit = true;
        (void) pthread_join(desktop.render_thread, NULL);
        desktop.render_thread_running = false;
    }
    ws_close(&desktop.ws);
    (void) sys_kbd_capture_set(0U);
    WS_LOG("shutdown\n");
    return 1;
}
