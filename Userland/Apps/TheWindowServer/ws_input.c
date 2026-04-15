#include "Includes/ws_input.h"
#include "Includes/ws_compositor.h"
#include "Includes/ws_ipc_server.h"
#include "Includes/ws_focus.h"
#include "Includes/ws_desktop_ui.h"

#include <string.h>
#include <signal.h>
#include <syscall.h>

static inline bool ws_window_fetch(const ws_context_t* ws, uint32_t window_id, ws_window_t* out)
{
    if (!ws || window_id == 0U || !out)
        return false;
    return ws_find_window(ws, window_id, out) == 0;
}

bool ws_point_in_window_raw(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!window || !window->visible)
        return false;

    int32_t x0 = window->x;
    int32_t y0 = window->y;
    int32_t x1 = x0 + (int32_t) window->width;
    int32_t y1 = y0 + (int32_t) window->height;
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

bool ws_point_in_titlebar(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!ws_point_in_window_raw(window, x, y))
        return false;

    int32_t y1 = window->y + (int32_t) WS_TITLEBAR_HEIGHT;
    return y >= window->y && y < y1;
}

bool ws_point_in_close_button(const ws_window_t* window, int32_t x, int32_t y)
{
    if (!window || !window->frame_controls)
        return false;
    if (!ws_point_in_titlebar(window, x, y))
        return false;
    if (window->width <= (WS_CLOSE_BUTTON_SIZE + (WS_CLOSE_BUTTON_MARGINX * 2U)))
        return false;

    int32_t bx = window->x + (int32_t) window->width
                 - (int32_t) WS_CLOSE_BUTTON_MARGINX - (int32_t) WS_CLOSE_BUTTON_SIZE;
    int32_t by = window->y + (int32_t) WS_CLOSE_BUTTON_MARGINY;
    int32_t bx2 = bx + (int32_t) WS_CLOSE_BUTTON_SIZE;
    int32_t by2 = by + (int32_t) WS_CLOSE_BUTTON_SIZE;
    return x >= bx && x < bx2 && y >= by && y < by2;
}

bool ws_point_in_dock_area(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop)
        return false;

    ws_window_t bar;
    ws_ctx_lock(desktop);
    bool ok = ws_window_fetch(&desktop->ws, desktop->dock_bar_id, &bar);
    ws_ctx_unlock(desktop);
    if (!ok)
        return false;

    return ws_point_in_window_raw(&bar, x, y);
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

static void ws_begin_drag_if_title(ws_desktop_t* desktop, uint32_t window_id,
                                   const ws_window_t* window, int32_t x, int32_t y)
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

static bool ws_handle_drag_motion(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop || !desktop->dragging || desktop->drag_window_id == 0U)
        return false;

    ws_ctx_lock(desktop);
    ws_window_t window;
    if (!ws_window_fetch(&desktop->ws, desktop->drag_window_id, &window))
    {
        ws_ctx_unlock(desktop);
        desktop->dragging = false;
        desktop->drag_window_id = 0U;
        return false;
    }

    int32_t nx = x - desktop->drag_offset_x;
    int32_t ny = y - desktop->drag_offset_y;

    int32_t max_x = (int32_t) desktop->ws.mode.hdisplay
                    - (int32_t) ws_min_u32(window.width, (uint32_t) desktop->ws.mode.hdisplay);
    int32_t max_y = (int32_t) desktop->ws.mode.vdisplay
                    - (int32_t) ws_min_u32(window.height, (uint32_t) desktop->ws.mode.vdisplay);
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > max_x)
        nx = max_x;
    if (ny > max_y)
        ny = max_y;

    if (nx == window.x && ny == window.y)
    {
        ws_ctx_unlock(desktop);
        return false;
    }

    ws_dirty_region_add_rect(desktop, window.x, window.y, window.width, window.height);
    ws_dirty_region_add_rect(desktop, nx, ny, window.width, window.height);
    (void) ws_move_window(&desktop->ws, desktop->drag_window_id, nx, ny);
    ws_ctx_unlock(desktop);
    return true;
}

void ws_request_close_window(ws_desktop_t* desktop, uint32_t slot, uint32_t window_id)
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

static void ws_handle_left_press(ws_desktop_t* desktop, int32_t x, int32_t y)
{
    if (!desktop)
        return;

    if (desktop->power_menu_open)
    {
        (void) ws_handle_power_click(desktop, x, y);
        return;
    }

    if (ws_point_in_dock_area(desktop, x, y))
    {
        bool keep_client_focus = ws_handle_dock_click(desktop, x, y);
        if (!keep_client_focus)
            ws_clear_focus(desktop);
        return;
    }

    uint32_t window_id = 0U;
    uint32_t slot = 0U;
    ws_window_t window;
    bool hit_client = false;

    ws_ctx_lock(desktop);
    for (uint32_t i = desktop->ws.window_count; i > 0U; i--)
    {
        ws_window_t w = desktop->ws.windows[i - 1U];
        if (!ws_point_in_window_raw(&w, x, y))
            continue;
        uint32_t sl = 0U;
        if (!ws_find_client_by_window(desktop, w.id, &sl))
            continue;
        window_id = w.id;
        window = w;
        slot = sl;
        hit_client = true;
        break;
    }

    if (hit_client)
    {
        uint32_t top_id = 0U;
        if (desktop->ws.window_count > 0U)
            top_id = desktop->ws.windows[desktop->ws.window_count - 1U].id;
        if (window_id != top_id)
        {
            int rc = ws_raise_window(&desktop->ws, window_id);
            if (rc == 0)
                desktop->compositor_need_full = true;
        }
    }
    ws_ctx_unlock(desktop);

    if (hit_client)
    {
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

    bool keep_client_focus = ws_handle_dock_click(desktop, x, y);
    if (!keep_client_focus)
        ws_clear_focus(desktop);
}

bool ws_process_mouse_events(ws_desktop_t* desktop,
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
        if (sdx > WS_MOUSE_DELTA_EVENT_CLAMP)
            sdx = WS_MOUSE_DELTA_EVENT_CLAMP;
        else if (sdx < -WS_MOUSE_DELTA_EVENT_CLAMP)
            sdx = -WS_MOUSE_DELTA_EVENT_CLAMP;
        if (sdy > WS_MOUSE_DELTA_EVENT_CLAMP)
            sdy = WS_MOUSE_DELTA_EVENT_CLAMP;
        else if (sdy < -WS_MOUSE_DELTA_EVENT_CLAMP)
            sdy = -WS_MOUSE_DELTA_EVENT_CLAMP;

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

void ws_dispatch_key_events(ws_desktop_t* desktop)
{
    if (!desktop || desktop->focused_window_id == 0U)
        return;

    if (desktop->focused_slot >= WS_CLIENT_MAX)
        return;

    ws_client_conn_t* client = &desktop->clients[desktop->focused_slot];
    bool owns_focused = ws_client_owns_window(client, desktop->focused_window_id);
    if (!client->active || !owns_focused)
        return;

    for (uint32_t i = 0U; i < WS_KEY_EVENT_BUDGET_PER_TICK; i++)
    {
        int code = sys_kbd_get_scancode();
        if (code <= 0)
            break;

        ws_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = WS_EVENT_KEY;
        event.window_id = desktop->focused_window_id;
        event.key = code;
        (void) ws_client_event_push(client, &event);
    }
}
