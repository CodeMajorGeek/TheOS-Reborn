#ifndef _VGA_H
#define _VGA_H

#define VGA_BUFFER_ADDRESS  0xB8000

#define VGA_WIDTH   80
#define VGA_HEIGHT  25

#define CURSOR_CTRL 0x3D4
#define CURSOR_DATA 0x3D5

#define CURSOR_POS  0x0E

#include <stdint.h>

enum vga_color
{
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14
};

static uint16_t* VGA_BUFFER = (uint16_t*) VGA_BUFFER_ADDRESS;

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return fg | (bg << 4);
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color)
{
    return (uint16_t) c | (uint16_t) color << 8;
}

#endif