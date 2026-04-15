#ifndef WS_DESKTOP_UI_H
#define WS_DESKTOP_UI_H

#include "ws_types.h"

bool ws_init_desktop(ws_desktop_t* desktop);
void ws_apply_desktop_colors(ws_desktop_t* desktop);

void ws_set_power_menu_visible(ws_desktop_t* desktop, bool visible);
bool ws_handle_power_click(ws_desktop_t* desktop, int32_t x, int32_t y);
bool ws_handle_dock_click(ws_desktop_t* desktop, int32_t x, int32_t y);

int ws_launch_gui_app(const char* path);
void ws_reap_children(ws_desktop_t* desktop);
void ws_gui_autorespawn_if_stuck(ws_desktop_t* desktop, ws_client_role_t role,
                                 pid_t* tracked_pid, uint64_t* tracked_tick,
                                 const char* app_path, const char* app_name);

bool ws_raise_first_window_for_role(ws_desktop_t* desktop, ws_client_role_t role);

#endif
