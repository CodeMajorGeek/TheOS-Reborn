#ifndef _TTYVGA_H
#define _TTYVGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TTYVGA_runtime_state
{
    uint16_t* buffer;
    bool cursor_enabled;
} TTYVGA_runtime_state_t;

void TTYVGA_set_buffer(uint16_t* buffer);
void TTYVGA_init(uint8_t color);
void TTYVGA_clear(uint8_t color);
void TTYVGA_put_entry_at(char c, uint8_t color, size_t x, size_t y);
void TTYVGA_scroll_one_line(uint8_t color);
void TTYVGA_enable_cursor(bool enabled);
void TTYVGA_update_cursor(uint8_t x, uint8_t y);

#endif
