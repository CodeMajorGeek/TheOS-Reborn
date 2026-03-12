#ifndef _TTY_FRAMEBUFFER_H
#define _TTY_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TTY_PSF2_MAGIC                   0x864AB572U
#define TTYFB_BACKBUFFER_VIRT_BASE       0xFFFFFFFF70000000ULL
#define TTYFB_BACKBUFFER_VIRT_SIZE       (64ULL * 1024ULL * 1024ULL)
#define TTYFB_CURSOR_BLINK_PERIOD_TICKS  50U
#define TTYFB_CURSOR_THICKNESS_PX        2U

typedef struct __attribute__((packed)) TTY_psf2_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t num_glyph;
    uint32_t bytes_per_glyph;
    uint32_t height;
    uint32_t width;
} TTY_psf2_header_t;

typedef struct TTYFB_runtime_state
{
    bool ready;
    uintptr_t phys_addr;
    uintptr_t virt_addr;
    uint8_t* front;
    uint8_t* back;
    size_t size;
    bool double_buffer;
    bool back_uses_kmem;
    size_t back_page_count;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t bytes_per_pixel;
    uint8_t red_pos;
    uint8_t red_size;
    uint8_t green_pos;
    uint8_t green_size;
    uint8_t blue_pos;
    uint8_t blue_size;
    bool font_ready;
    uint8_t* font_bitmap;
    size_t font_bitmap_size;
    uint32_t font_width;
    uint32_t font_height;
    uint32_t font_num_glyph;
    uint32_t font_bytes_per_glyph;
    uint32_t font_row_bytes;
    uint32_t cols;
    uint32_t rows;
    bool cursor_enabled;
    bool cursor_drawn;
    uint32_t cursor_cell_x;
    uint32_t cursor_cell_y;
    uint32_t cursor_blink_ticks;
} TTYFB_runtime_state_t;

#endif
