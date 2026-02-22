#ifndef _TTY_H
#define _TTY_H

#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define USE_COM2_OUTPUT

#ifdef USE_COM2_OUTPUT
#include <Device/COM.h>
#define TTY_COM_PORT    COM2
#endif

typedef struct TTY_framebuffer_info
{
    uint64_t phys_addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
    uint8_t red_field_position;
    uint8_t red_mask_size;
    uint8_t green_field_position;
    uint8_t green_mask_size;
    uint8_t blue_field_position;
    uint8_t blue_mask_size;
} TTY_framebuffer_info_t;

void TTY_set_buffer(uint16_t* buffer);

void TTY_init(void);
bool TTY_init_framebuffer(const TTY_framebuffer_info_t* info);
bool TTY_has_framebuffer(void);
bool TTY_load_psf2(const uint8_t* data, size_t size);

void TTY_clear(void);
void TTY_set_color(uint8_t color);
void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y);

void TTY_putc(char c);
void TTY_write(const char* str, size_t len);
void TTY_puts(const char* str);

void TTY_enable_cursor(bool enabled);
void TTY_update_cursor(uint8_t x, uint8_t y);
void TTY_on_timer_tick(void);

#endif
