#ifndef _TTY_H
#define _TTY_H

#define USE_VGA_TEXT_MODE

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include <CPU/IO.h>

#ifdef USE_VGA_TEXT_MODE
#include <Device/VGA.h>
#endif

void TTY_init(void);

void TTY_clear(void);
void TTY_set_color(uint8_t color);
void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y);

void TTY_putc(char c);
void TTY_write(const char* str, size_t len);
void TTY_puts(const char* str);

void TTY_enable_cursor(bool enabled);
void TTY_update_cursor(uint8_t x, uint8_t y);

#endif