#include "Includes/ws_focus.h"
#include "Includes/ws_ipc_server.h"
#include "Includes/ws_compositor.h"

void ws_set_focus(ws_desktop_t* desktop, uint32_t window_id, uint32_t slot)
{
    if (!desktop)
        return;

    desktop->focused_window_id = window_id;
    desktop->focused_slot = slot;
}

void ws_clear_focus(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    desktop->focused_window_id = 0U;
    desktop->focused_slot = 0U;
}

bool ws_pick_top_client_window(const ws_desktop_t* desktop,
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

void ws_refresh_focus_after_close(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    if (desktop->focused_window_id == 0U)
        return;

    uint32_t window_id = 0U;
    uint32_t slot = 0U;
    ws_window_t window;
    bool picked = false;

    ws_ctx_lock(desktop);
    if (ws_pick_top_client_window(desktop, &window_id, &window, &slot))
        picked = true;
    ws_ctx_unlock(desktop);

    if (picked)
    {
        ws_set_focus(desktop, window_id, slot);
        return;
    }

    ws_clear_focus(desktop);
}
