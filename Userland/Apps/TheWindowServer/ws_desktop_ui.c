#include "Includes/ws_desktop_ui.h"
#include "Includes/ws_compositor.h"
#include "Includes/ws_ipc_server.h"
#include "Includes/ws_focus.h"
#include "Includes/ws_input.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/*  1. ws_create_window_checked                                               */
/* -------------------------------------------------------------------------- */

static int ws_create_window_checked(ws_context_t* ws, const ws_window_desc_t* desc, uint32_t* out_id)
{
    if (ws_create_window(ws, desc, out_id) < 0)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  2. ws_window_fetch                                                        */
/* -------------------------------------------------------------------------- */

static bool ws_window_fetch(const ws_context_t* ws, uint32_t window_id, ws_window_t* out)
{
    if (!ws || window_id == 0U || !out)
        return false;
    return ws_find_window(ws, window_id, out) == 0;
}

/* -------------------------------------------------------------------------- */
/*  3-6. Launch helpers                                                        */
/* -------------------------------------------------------------------------- */

static int ws_spawn_local(const char* path, char* const argv[])
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    char* fallback_argv[2] = { 0 };
    char* const* final_argv = argv;
    if (!final_argv)
    {
        const char* basename = path;
        for (const char* p = path; *p; p++)
        {
            if (*p == '/')
                basename = p + 1;
        }
        fallback_argv[0] = (char*) basename;
        fallback_argv[1] = NULL;
        final_argv = fallback_argv;
    }

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        (void) execv(path, final_argv);
        _exit(127);
    }

    return 0;
}

static int ws_launch_powermanager(const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }
    char* argv[] = { (char*) "ThePowerManager", (char*) arg, NULL };
    return ws_spawn_local("/bin/ThePowerManager", argv);
}

int ws_launch_gui_app(const char* path)
{
    return ws_spawn_local(path, NULL);
}

/* -------------------------------------------------------------------------- */
/*  11. ws_set_power_menu_visible                                             */
/* -------------------------------------------------------------------------- */

void ws_set_power_menu_visible(ws_desktop_t* desktop, bool visible)
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

/* -------------------------------------------------------------------------- */
/*  12. ws_execute_power_action                                               */
/* -------------------------------------------------------------------------- */

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
        ws_set_power_menu_visible(desktop, false);
        (void) ws_launch_powermanager("-s");
        (void) shutdown();
        return;
    }

    if (action == WS_POWER_ITEM_RESTART)
    {
        ws_set_power_menu_visible(desktop, false);
        (void) ws_launch_powermanager("-r");
        (void) reboot();
    }
}

/* -------------------------------------------------------------------------- */
/*  13. ws_apply_desktop_colors                                               */
/* -------------------------------------------------------------------------- */

void ws_apply_desktop_colors(ws_desktop_t* desktop)
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
        const uint32_t power_focus_colors[WS_POWER_ITEM_COUNT] = {
            0x00D78383U,
            0x00A8C0E3U,
            0x00A2B8CBU
        };

        for (uint32_t i = 0U; i < WS_POWER_ITEM_COUNT; i++)
        {
            uint32_t color = (desktop->power_focus == (ws_power_item_t) i) ? power_focus_colors[i] : power_base[i];
            (void) ws_set_window_color(&desktop->ws, desktop->power_button_ids[i], color);
        }
    }
}

bool ws_update_top_clock(ws_desktop_t* desktop)
{
    if (!desktop || desktop->top_bar_id == 0U)
        return false;

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return false;

    uint64_t now_sec = (uint64_t) tv.tv_sec;
    if (desktop->top_clock_last_sec == now_sec)
        return false;

    char clock_text[80];
    time_t now_time = (time_t) tv.tv_sec;
    struct tm* tm_now = localtime(&now_time);
    if (!tm_now)
        return false;
    if (strftime(clock_text, sizeof(clock_text), "%d/%m/%Y %H:%M:%S", tm_now) == 0U)
        return false;

    char title[128];
    (void) snprintf(title, sizeof(title), "TheOS WindowServer  %s", clock_text);
    if (ws_set_window_text(&desktop->ws, desktop->top_bar_id, title) == 0)
    {
        ws_desktop_invalidate_window_id(desktop, desktop->top_bar_id);
        desktop->top_clock_last_sec = now_sec;
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/*  14. ws_handle_power_click                                                 */
/* -------------------------------------------------------------------------- */

bool ws_handle_power_click(ws_desktop_t* desktop, int32_t x, int32_t y)
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

/* -------------------------------------------------------------------------- */
/*  15-16. Autorespawn via launcher: track launch ticks, not PIDs              */
/* -------------------------------------------------------------------------- */

void ws_gui_autorespawn_if_stuck(ws_desktop_t* desktop,
                                 ws_client_role_t role,
                                 pid_t* tracked_pid,
                                 uint64_t* tracked_tick,
                                 const char* app_path,
                                 const char* app_name)
{
    if (!desktop || !tracked_pid || !tracked_tick || !app_path || !app_name)
        return;
    if (*tracked_tick == UINT64_MAX)
        return;

    if (ws_role_ipc_session_open(desktop, role) || ws_role_has_live_client(desktop, role))
        return;

    uint64_t now = sys_tick_get();
    uint64_t elapsed = ws_tick_delta(now, *tracked_tick);
    if (elapsed < WS_GUI_SPAWN_HANDSHAKE_GRACE_TICKS)
        return;

    WS_LOG("autorespawn role=%u elapsed=%llu\n",
           (unsigned int) role,
           (unsigned long long) elapsed);
    if (ws_launch_gui_app(app_path) < 0)
    {
        WS_LOG("autorespawn failed role=%u errno=%d\n",
               (unsigned int) role, errno);
        return;
    }
    *tracked_pid = (pid_t) 1;
    *tracked_tick = now;
    WS_LOG("autorespawn launched role=%u\n", (unsigned int) role);
}

/* -------------------------------------------------------------------------- */
/*  17. ws_handle_dock_click                                                  */
/* -------------------------------------------------------------------------- */

bool ws_handle_dock_click(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop)
        return false;

    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        ws_window_t icon;
        if (!ws_window_fetch(&desktop->ws, desktop->dock_icon_ids[i], &icon))
            continue;
        if (!ws_point_in_window_raw(&icon, x, y))
            continue;

        if (i == WS_DOCK_ITEM_SHELL)
        {
            WS_LOG("dock click: launch TheShellGUI\n");
            if (ws_launch_gui_app("/bin/TheShellGUI") < 0)
            {
                WS_LOG("dock launch TheShellGUI failed errno=%d\n", errno);
                return false;
            }
            desktop->shell_gui_spawn_pid = (pid_t) 1;
            desktop->shell_gui_spawn_tick = sys_tick_get();
            ws_desktop_invalidate_chrome(desktop);
            return false;
        }
        else if (i == WS_DOCK_ITEM_SYSTEM_MONITOR)
        {
            WS_LOG("dock click: launch TheSystemMonitorGUI\n");
            if (ws_launch_gui_app("/bin/TheSystemMonitorGUI") < 0)
            {
                WS_LOG("dock launch TheSystemMonitorGUI failed errno=%d\n", errno);
                return false;
            }
            desktop->monitor_gui_spawn_pid = (pid_t) 1;
            desktop->monitor_gui_spawn_tick = sys_tick_get();
            ws_desktop_invalidate_chrome(desktop);
            return false;
        }
        else if (i == WS_DOCK_ITEM_POWER)
        {
            WS_LOG("dock click: open power menu\n");
            ws_set_power_menu_visible(desktop, true);
        }

        return false;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/*  18. ws_raise_first_window_for_role                                        */
/* -------------------------------------------------------------------------- */

bool ws_raise_first_window_for_role(ws_desktop_t* desktop, ws_client_role_t role)
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
        {
            ws_set_focus(desktop, window_id, i);
            return true;
        }
        ws_ctx_lock(desktop);
        int rc_raise = ws_raise_window(&desktop->ws, window_id);
        ws_ctx_unlock(desktop);
        if (rc_raise != 0)
            return false;
        desktop->compositor_need_full = true;
        ws_set_focus(desktop, window_id, i);
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/*  19. ws_reap_children                                                      */
/* -------------------------------------------------------------------------- */

void ws_reap_children(ws_desktop_t* desktop)
{
    for (uint32_t i = 0U; i < 256U; i++)
    {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            return;
        if (desktop)
        {
            if (desktop->shell_gui_spawn_pid == pid)
                desktop->shell_gui_spawn_pid = (pid_t) -1;
            if (desktop->monitor_gui_spawn_pid == pid)
                desktop->monitor_gui_spawn_pid = (pid_t) -1;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  20. ws_init_desktop                                                       */
/* -------------------------------------------------------------------------- */

bool ws_init_desktop(ws_desktop_t* desktop)
{
    if (!desktop)
        return false;

    memset(desktop, 0, sizeof(*desktop));
    desktop->listen_fd = -1;
    desktop->shell_gui_spawn_pid = (pid_t) -1;
    desktop->shell_gui_spawn_tick = UINT64_MAX;
    desktop->monitor_gui_spawn_pid = (pid_t) -1;
    desktop->monitor_gui_spawn_tick = UINT64_MAX;
    desktop->top_clock_last_sec = UINT64_MAX;
    desktop->focused_window_id = 0U;
    desktop->focused_slot = 0U;
    for (uint32_t i = 0U; i < WS_CLIENT_MAX; i++)
        ws_client_reset_slot(&desktop->clients[i]);

    (void) pthread_mutex_init(&desktop->render_mutex, NULL);
    (void) pthread_mutex_init(&desktop->ws_mutex, NULL);

    if (ws_open(&desktop->ws, true) < 0)
    {
        WS_LOG("ws_open failed errno=%d\n", errno);
        return false;
    }

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
        .title = ""
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

        ws_window_desc_t icon_desc = {
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

        if (ws_create_window_checked(&desktop->ws, &icon_desc, &desktop->dock_icon_ids[i]) < 0)
        {
            WS_LOG("dock icon create failed i=%u errno=%d\n", (unsigned int) i, errno);
            ws_close(&desktop->ws);
            return false;
        }
    }

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

    desktop->listen_fd = ws_server_socket_open();
    if (desktop->listen_fd < 0)
    {
        WS_LOG("desktop server socket open failed errno=%d\n", errno);
        ws_close(&desktop->ws);
        return false;
    }

    ws_set_power_menu_visible(desktop, false);
    ws_apply_desktop_colors(desktop);
    ws_update_top_clock(desktop);
    ws_dirty_region_reset(desktop);
    WS_LOG("desktop init complete top=%u dock=%u\n",
           (unsigned int) desktop->top_bar_id,
           (unsigned int) desktop->dock_bar_id);
    return true;
}
