#ifndef WS_FOCUS_H
#define WS_FOCUS_H

#include "ws_types.h"

void ws_set_focus(ws_desktop_t* desktop, uint32_t window_id, uint32_t slot);
void ws_clear_focus(ws_desktop_t* desktop);
void ws_refresh_focus_after_close(ws_desktop_t* desktop);
bool ws_pick_top_client_window(const ws_desktop_t* desktop, uint32_t* out_window_id,
                               ws_window_t* out_window, uint32_t* out_slot);

#endif
