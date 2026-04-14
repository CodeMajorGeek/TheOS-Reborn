#ifndef _MOUSE_H
#define _MOUSE_H

#include <CPU/ISR.h>
#include <UAPI/Syscall.h>
#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stdint.h>

#define MOUSE_EVENT_BUFFER_SIZE 256U
#define MOUSE_BYTE_SOURCE_IRQ12 1U
#define MOUSE_BYTE_SOURCE_IRQ1  2U
#define MOUSE_BYTE_SOURCE_POLL  3U

typedef struct Mouse_runtime_state
{
    syscall_mouse_event_t events[MOUSE_EVENT_BUFFER_SIZE];
    uint16_t event_count;
    uint16_t write_pos;
    uint16_t read_pos;
    uint8_t packet[3];
    uint8_t packet_index;
    spinlock_t lock;
    uint64_t irq12_callbacks;
    uint64_t irq12_bytes_total;
    uint64_t irq12_aux_bytes;
    uint64_t irq12_non_aux_bytes;
    uint64_t irq12_drain_budget_hits;
    uint64_t irq1_aux_bytes;
    uint64_t poll_cycles;
    uint64_t poll_bytes_total;
    uint64_t poll_aux_bytes;
    uint64_t poll_non_aux_bytes;
    uint64_t events_pushed;
    uint64_t events_dropped_full;
    uint64_t events_popped;
    uint64_t get_event_empty;
    uint64_t packet_sync_drops;
    uint64_t packet_overflow_drops;
    uint64_t forced_request_attempts;
    uint64_t forced_request_success;
    uint64_t forced_request_fail;
    uint64_t last_forced_request_tick;
    uint8_t last_buttons;
    bool last_buttons_valid;
    bool ready;
} Mouse_runtime_state_t;

void Mouse_init(void);
bool Mouse_get_event(syscall_mouse_event_t* out_event);
bool Mouse_is_ready(void);
bool Mouse_handle_controller_byte(uint8_t status, uint8_t byte, uint8_t source);
bool Mouse_get_debug_info(syscall_mouse_debug_info_t* out_info);

#endif
