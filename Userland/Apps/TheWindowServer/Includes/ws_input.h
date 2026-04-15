#ifndef WS_INPUT_H
#define WS_INPUT_H

#include "ws_types.h"

bool ws_point_in_window_raw(const ws_window_t* window, int32_t x, int32_t y);
bool ws_point_in_titlebar(const ws_window_t* window, int32_t x, int32_t y);
bool ws_point_in_close_button(const ws_window_t* window, int32_t x, int32_t y);
bool ws_point_in_dock_area(ws_desktop_t* desktop, int32_t x, int32_t y);

bool ws_process_mouse_events(ws_desktop_t* desktop, int32_t* cursor_x, int32_t* cursor_y,
                             uint8_t* mouse_buttons, bool* mouse_state_init,
                             bool* out_cursor_dirty, bool* out_layout_dirty,
                             bool* out_layout_full_dirty, uint32_t budget);
void ws_dispatch_key_events(ws_desktop_t* desktop);

void ws_request_close_window(ws_desktop_t* desktop, uint32_t slot, uint32_t window_id);

#endif
