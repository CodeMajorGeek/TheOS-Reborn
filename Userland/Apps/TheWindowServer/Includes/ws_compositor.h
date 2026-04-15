#ifndef WS_COMPOSITOR_H
#define WS_COMPOSITOR_H

#include "ws_types.h"

void ws_ctx_lock(ws_desktop_t* desktop);
void ws_ctx_unlock(ws_desktop_t* desktop);
void ws_render_signal(ws_desktop_t* desktop);

bool ws_tiles_init(ws_desktop_t* desktop);
void ws_tiles_mark_all(ws_desktop_t* desktop);
void ws_tiles_clear(ws_desktop_t* desktop);
void ws_tiles_mark_rect(ws_desktop_t* desktop, int32_t x, int32_t y, uint32_t width, uint32_t height);
bool ws_tiles_pop_all_bbox(ws_desktop_t* desktop,
                           int32_t* out_x, int32_t* out_y,
                           uint32_t* out_w, uint32_t* out_h,
                           bool* out_full_screen);

void* ws_render_thread_main(void* arg);

void ws_dirty_region_reset(ws_desktop_t* desktop);
void ws_dirty_region_add_rect(ws_desktop_t* desktop, int32_t x, int32_t y,
                              uint32_t width, uint32_t height);
bool ws_dirty_region_take(ws_desktop_t* desktop, int32_t* out_x, int32_t* out_y,
                          uint32_t* out_width, uint32_t* out_height);

void ws_mark_cursor_move_tiles(ws_desktop_t* desktop, int32_t old_x, int32_t old_y,
                               int32_t new_x, int32_t new_y);
bool ws_try_flush_cursor_tiles_early(ws_desktop_t* desktop, int32_t cursor_x, int32_t cursor_y,
                                     int32_t* rendered_cursor_x, int32_t* rendered_cursor_y,
                                     bool* cursor_dirty, bool layout_dirty,
                                     bool layout_full_dirty, uint64_t now_tick,
                                     uint64_t first_render_tick);

void ws_desktop_invalidate_window_id(ws_desktop_t* desktop, uint32_t window_id);
bool ws_invalidate_window_text_delta(ws_desktop_t* desktop, const ws_window_t* window,
                                     const char* old_text, const char* new_text);
void ws_desktop_invalidate_chrome(ws_desktop_t* desktop);

#endif
