#ifndef WS_IPC_SERVER_H
#define WS_IPC_SERVER_H

#include "ws_types.h"

int ws_server_socket_open(void);

void ws_client_reset_slot(ws_client_conn_t* client);
bool ws_client_add_window(ws_client_conn_t* client, uint32_t window_id);
void ws_client_remove_window(ws_client_conn_t* client, uint32_t window_id);
bool ws_client_owns_window(const ws_client_conn_t* client, uint32_t window_id);
bool ws_find_client_by_window(const ws_desktop_t* desktop, uint32_t window_id, uint32_t* out_slot);
bool ws_client_event_push(ws_client_conn_t* client, const ws_event_t* event);
bool ws_client_event_pop(ws_client_conn_t* client, ws_event_t* out_event);
void ws_client_destroy_all_windows(ws_desktop_t* desktop, ws_client_conn_t* client);
void ws_client_disconnect_slot(ws_desktop_t* desktop, uint32_t slot);

int ws_ipc_send_response(int fd, uint16_t opcode, int32_t status,
                         uint32_t window_id, const void* payload, uint32_t payload_len);

bool ws_accept_clients(ws_desktop_t* desktop);
bool ws_poll_clients(ws_desktop_t* desktop, bool* out_activity);

uint32_t ws_active_client_count(const ws_desktop_t* desktop);
uint32_t ws_active_client_windows(const ws_desktop_t* desktop);
bool ws_role_has_live_client(const ws_desktop_t* desktop, ws_client_role_t role);
bool ws_role_ipc_session_open(const ws_desktop_t* desktop, ws_client_role_t role);

#endif
