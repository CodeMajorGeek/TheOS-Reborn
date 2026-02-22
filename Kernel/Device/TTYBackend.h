#ifndef _TTY_BACKEND_H
#define _TTY_BACKEND_H

#include <Device/TTY.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void TTYVGA_set_buffer(uint16_t* buffer);
void TTYVGA_init(uint8_t color);
void TTYVGA_clear(uint8_t color);
void TTYVGA_put_entry_at(char c, uint8_t color, size_t x, size_t y);
void TTYVGA_scroll_one_line(uint8_t color);
void TTYVGA_enable_cursor(bool enabled);
void TTYVGA_update_cursor(uint8_t x, uint8_t y);

bool TTYFB_load_psf2(const uint8_t* data, size_t size);
bool TTYFB_init(const TTY_framebuffer_info_t* info, uint8_t color);
bool TTYFB_is_ready(void);
void TTYFB_clear(uint8_t color);
void TTYFB_put_entry_at(char c, uint8_t color, size_t x, size_t y);
void TTYFB_scroll_one_line(uint8_t color);
void TTYFB_enable_cursor(bool enabled);
void TTYFB_update_cursor(uint8_t x, uint8_t y);
void TTYFB_on_timer_tick(void);
void TTYFB_get_grid(uint32_t* cols, uint32_t* rows);

#endif
