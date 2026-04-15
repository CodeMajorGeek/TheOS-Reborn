#include "Includes/ws_compositor.h"
#include "Includes/ws_ipc_server.h"

#include <sched.h>
#include <string.h>
#include <stdlib.h>

void ws_ctx_lock(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_lock(&desktop->ws_mutex);
}

void ws_ctx_unlock(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_unlock(&desktop->ws_mutex);
}

void ws_render_signal(ws_desktop_t* desktop)
{
    if (!desktop)
        return;
    (void) pthread_mutex_lock(&desktop->render_mutex);
    (void) pthread_mutex_unlock(&desktop->render_mutex);
}

bool ws_tiles_init(ws_desktop_t* desktop)
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

void ws_tiles_mark_all(ws_desktop_t* desktop)
{
    if (!desktop || !desktop->dirty_tiles)
        return;

    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    for (uint32_t i = 0U; i < total; i++)
        desktop->dirty_tiles[i] = 1U;
    desktop->dirty_tile_count = total;
}

void ws_tiles_clear(ws_desktop_t* desktop)
{
    if (!desktop || !desktop->dirty_tiles)
        return;
    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    memset(desktop->dirty_tiles, 0, (size_t) total);
    desktop->dirty_tile_count = 0U;
}

void ws_tiles_mark_rect(ws_desktop_t* desktop,
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

bool ws_tiles_pop_all_bbox(ws_desktop_t* desktop,
                           int32_t* out_x,
                           int32_t* out_y,
                           uint32_t* out_w,
                           uint32_t* out_h,
                           bool* out_full_screen)
{
    if (!desktop || !desktop->dirty_tiles || desktop->dirty_tile_count == 0U ||
        !out_x || !out_y || !out_w || !out_h || !out_full_screen)
        return false;

    uint32_t total = desktop->tiles_w * desktop->tiles_h;
    bool found = false;
    int32_t bx0 = 0;
    int32_t by0 = 0;
    int32_t bx1 = 0;
    int32_t by1 = 0;
    uint32_t consumed = 0U;

    for (uint32_t idx = 0U; idx < total; idx++)
    {
        if (desktop->dirty_tiles[idx] == 0U)
            continue;

        uint32_t tx = idx % desktop->tiles_w;
        uint32_t ty = idx / desktop->tiles_w;
        int32_t px = (int32_t) (tx * WS_RENDER_TILE_W);
        int32_t py = (int32_t) (ty * WS_RENDER_TILE_H);
        uint32_t w = WS_RENDER_TILE_W;
        uint32_t h = WS_RENDER_TILE_H;
        if ((uint32_t) px + w > desktop->ws.mode.hdisplay)
            w = desktop->ws.mode.hdisplay - (uint32_t) px;
        if ((uint32_t) py + h > desktop->ws.mode.vdisplay)
            h = desktop->ws.mode.vdisplay - (uint32_t) py;

        if (!found)
        {
            bx0 = px;
            by0 = py;
            bx1 = px + (int32_t) w;
            by1 = py + (int32_t) h;
        }
        else
        {
            if (px < bx0) bx0 = px;
            if (py < by0) by0 = py;
            if (px + (int32_t) w > bx1) bx1 = px + (int32_t) w;
            if (py + (int32_t) h > by1) by1 = py + (int32_t) h;
        }
        found = true;
        desktop->dirty_tiles[idx] = 0U;
        consumed++;
    }

    desktop->dirty_tile_count = 0U;

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
    *out_full_screen = (consumed == total);
    return (*out_w != 0U && *out_h != 0U);
}

void* ws_render_thread_main(void* arg)
{
    ws_desktop_t* desktop = (ws_desktop_t*) arg;
    if (!desktop)
        return NULL;

    for (;;)
    {
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
            (void) sched_yield();
            continue;
        }

        if (!desktop->render_ctx_snapshot)
            continue;

        if (do_full)
        {
            (void) pthread_mutex_lock(&desktop->render_mutex);
            ws_tiles_mark_all(desktop);
            (void) pthread_mutex_unlock(&desktop->render_mutex);
        }

        int32_t rx = 0;
        int32_t ry = 0;
        uint32_t rw = 0U;
        uint32_t rh = 0U;
        bool full_screen = false;

        (void) pthread_mutex_lock(&desktop->render_mutex);
        bool has_region = ws_tiles_pop_all_bbox(desktop, &rx, &ry, &rw, &rh, &full_screen);
        (void) pthread_mutex_unlock(&desktop->render_mutex);

        if (!has_region)
            continue;

        ws_ctx_lock(desktop);
        uint64_t render_start = sys_tick_get();
        int rc;
        if (full_screen)
            rc = ws_render(&desktop->ws);
        else
            rc = ws_render_region(&desktop->ws, rx, ry, rw, rh);
        uint64_t render_end = sys_tick_get();
        ws_ctx_unlock(desktop);

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

        (void) sched_yield();
    }

    return NULL;
}

void ws_dirty_region_reset(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    desktop->dirty_region_valid = false;
    desktop->dirty_x0 = 0;
    desktop->dirty_y0 = 0;
    desktop->dirty_x1 = 0;
    desktop->dirty_y1 = 0;
}

void ws_dirty_region_add_rect(ws_desktop_t* desktop,
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

bool ws_dirty_region_take(ws_desktop_t* desktop,
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

void ws_mark_cursor_move_tiles(ws_desktop_t* desktop,
                               int32_t old_x,
                               int32_t old_y,
                               int32_t new_x,
                               int32_t new_y)
{
    if (!desktop)
        return;

    uint32_t box_w = (uint32_t) ((int32_t) WS_CURSOR_WIDTH + (int32_t) WS_CURSOR_SHADOW_OFFSET);
    uint32_t box_h = (uint32_t) ((int32_t) WS_CURSOR_HEIGHT + (int32_t) WS_CURSOR_SHADOW_OFFSET);

    (void) pthread_mutex_lock(&desktop->render_mutex);
    ws_tiles_mark_rect(desktop, old_x, old_y, box_w, box_h);
    ws_tiles_mark_rect(desktop, new_x, new_y, box_w, box_h);
    (void) pthread_mutex_unlock(&desktop->render_mutex);
}

bool ws_try_flush_cursor_tiles_early(ws_desktop_t* desktop,
                                     int32_t cursor_x,
                                     int32_t cursor_y,
                                     int32_t* rendered_cursor_x,
                                     int32_t* rendered_cursor_y,
                                     bool* cursor_dirty,
                                     bool layout_dirty,
                                     bool layout_full_dirty,
                                     uint64_t now_tick,
                                     uint64_t first_render_tick)
{
    if (!desktop || !rendered_cursor_x || !rendered_cursor_y || !cursor_dirty)
        return false;
    if (!*cursor_dirty || layout_dirty || layout_full_dirty || now_tick < first_render_tick)
        return false;

    if (*rendered_cursor_x == cursor_x && *rendered_cursor_y == cursor_y)
        return false;

    int32_t ox = *rendered_cursor_x;
    int32_t oy = *rendered_cursor_y;
    ws_mark_cursor_move_tiles(desktop, ox, oy, cursor_x, cursor_y);

    *rendered_cursor_x = cursor_x;
    *rendered_cursor_y = cursor_y;
    *cursor_dirty = false;

    return true;
}

void ws_desktop_invalidate_window_id(ws_desktop_t* desktop, uint32_t window_id)
{
    if (!desktop || window_id == 0U)
        return;

    ws_window_t w;
    ws_ctx_lock(desktop);
    int rc = ws_find_window(&desktop->ws, window_id, &w);
    ws_ctx_unlock(desktop);
    if (rc == 0)
        ws_dirty_region_add_rect(desktop, w.x, w.y, w.width, w.height);
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

bool ws_invalidate_window_text_delta(ws_desktop_t* desktop,
                                     const ws_window_t* window,
                                     const char* old_text,
                                     const char* new_text)
{
    if (!desktop || !window || !old_text || !new_text)
        return false;
    if (!desktop->ws.font_ready || desktop->ws.font_height == 0U)
        return false;

    if (ws_window_body_text_equal(old_text, new_text))
        return true;

    const size_t cap = (size_t) WS_WINDOW_BODY_TEXT_MAX + 1U;
    size_t old_len = ws_window_body_strlen_bounded(old_text, cap);
    size_t new_len = ws_window_body_strlen_bounded(new_text, cap);
    size_t first_diff = 0U;
    while (first_diff < old_len &&
           first_diff < new_len &&
           old_text[first_diff] == new_text[first_diff])
        first_diff++;

    size_t old_end = old_len;
    size_t new_end = new_len;
    while (old_end > first_diff &&
           new_end > first_diff &&
           old_text[old_end - 1U] == new_text[new_end - 1U])
    {
        old_end--;
        new_end--;
    }

    if (old_end == first_diff && new_end == first_diff)
        return true;

    uint32_t start_line_new = ws_text_line_index_at(new_text, first_diff);
    uint32_t start_line_old = ws_text_line_index_at(old_text, first_diff);
    uint32_t start_line = (start_line_new < start_line_old) ? start_line_new : start_line_old;

    size_t end_chr_new = (new_end > first_diff) ? (new_end - 1U) : first_diff;
    size_t end_chr_old = (old_end > first_diff) ? (old_end - 1U) : first_diff;
    uint32_t end_line_new = ws_text_line_index_at(new_text, end_chr_new);
    uint32_t end_line_old = ws_text_line_index_at(old_text, end_chr_old);
    uint32_t end_line = (end_line_new > end_line_old) ? end_line_new : end_line_old;
    if (end_line < start_line)
        end_line = start_line;

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

void ws_desktop_invalidate_chrome(ws_desktop_t* desktop)
{
    if (!desktop)
        return;

    /* Ne pas passer par dirty_region : add_rect fusionnerait barre + dock en un AABB
     * quasi plein écran et un seul gros ws_render_region. */
    ws_window_t w;
    ws_ctx_lock(desktop);
    (void) pthread_mutex_lock(&desktop->render_mutex);
    if (ws_find_window(&desktop->ws, desktop->top_bar_id, &w) == 0)
        ws_tiles_mark_rect(desktop, w.x, w.y, w.width, w.height);
    if (ws_find_window(&desktop->ws, desktop->dock_bar_id, &w) == 0)
        ws_tiles_mark_rect(desktop, w.x, w.y, w.width, w.height);
    for (uint32_t i = 0U; i < WS_DOCK_ICON_COUNT; i++)
    {
        if (ws_find_window(&desktop->ws, desktop->dock_icon_ids[i], &w) == 0)
            ws_tiles_mark_rect(desktop, w.x, w.y, w.width, w.height);
    }
    (void) pthread_mutex_unlock(&desktop->render_mutex);
    ws_ctx_unlock(desktop);
}
