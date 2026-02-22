#include "TTYBackend.h"

#include <Multiboot2/multiboot2.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Debug/KDebug.h>
#include <Device/VGA.h>

#include <stdint.h>
#include <string.h>

#define TTY_PSF2_MAGIC 0x864AB572U

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

static bool TTYFB_ready = false;
static uintptr_t TTYFB_phys_addr = 0;
static uintptr_t TTYFB_virt_addr = 0;
static uint8_t* TTYFB_front = NULL;
static uint8_t* TTYFB_back = NULL;
static size_t TTYFB_size = 0;
static bool TTYFB_double_buffer = false;
static bool TTYFB_back_uses_kmem = false;
static size_t TTYFB_back_page_count = 0;
static uint32_t TTYFB_width = 0;
static uint32_t TTYFB_height = 0;
static uint32_t TTYFB_pitch = 0;
static uint8_t TTYFB_bpp = 0;
static uint8_t TTYFB_bytes_per_pixel = 0;
static uint8_t TTYFB_red_pos = 0;
static uint8_t TTYFB_red_size = 0;
static uint8_t TTYFB_green_pos = 0;
static uint8_t TTYFB_green_size = 0;
static uint8_t TTYFB_blue_pos = 0;
static uint8_t TTYFB_blue_size = 0;

static bool TTYFB_font_ready = false;
static uint8_t* TTYFB_font_bitmap = NULL;
static size_t TTYFB_font_bitmap_size = 0;
static uint32_t TTYFB_font_width = 0;
static uint32_t TTYFB_font_height = 0;
static uint32_t TTYFB_font_num_glyph = 0;
static uint32_t TTYFB_font_bytes_per_glyph = 0;
static uint32_t TTYFB_font_row_bytes = 0;

static uint32_t TTYFB_cols = VGA_WIDTH;
static uint32_t TTYFB_rows = VGA_HEIGHT;
static bool TTYFB_cursor_enabled = false;
static bool TTYFB_cursor_drawn = false;
static uint32_t TTYFB_cursor_cell_x = 0;
static uint32_t TTYFB_cursor_cell_y = 0;
static uint32_t TTYFB_cursor_blink_ticks = 0;

#define TTYFB_BACKBUFFER_VIRT_BASE 0xFFFFFFFF70000000ULL
#define TTYFB_BACKBUFFER_VIRT_SIZE (64ULL * 1024ULL * 1024ULL)
#define TTYFB_CURSOR_BLINK_PERIOD_TICKS 50U
#define TTYFB_CURSOR_THICKNESS_PX 2U

static const uint8_t TTYFB_vga_palette[16][3] =
{
    { 0x00, 0x00, 0x00 }, // black
    { 0x00, 0x00, 0xAA }, // blue
    { 0x00, 0xAA, 0x00 }, // green
    { 0x00, 0xAA, 0xAA }, // cyan
    { 0xAA, 0x00, 0x00 }, // red
    { 0xAA, 0x00, 0xAA }, // magenta
    { 0xAA, 0x55, 0x00 }, // brown
    { 0xAA, 0xAA, 0xAA }, // light grey
    { 0x55, 0x55, 0x55 }, // dark grey
    { 0x55, 0x55, 0xFF }, // light blue
    { 0x55, 0xFF, 0x55 }, // light green
    { 0x55, 0xFF, 0xFF }, // light cyan
    { 0xFF, 0x55, 0x55 }, // light red
    { 0xFF, 0x55, 0xFF }, // light magenta
    { 0xFF, 0xFF, 0x55 }, // yellow
    { 0xFF, 0xFF, 0xFF }  // white
};

static uint32_t TTYFB_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static void TTYFB_release_backbuffer(void)
{
    if (!TTYFB_back)
        return;

    if (TTYFB_back_uses_kmem)
    {
        kfree(TTYFB_back);
    }
    else if (TTYFB_back_page_count != 0)
    {
        uintptr_t virt = TTYFB_BACKBUFFER_VIRT_BASE;
        for (size_t i = 0; i < TTYFB_back_page_count; i++)
            (void) VMM_unmap_page(virt + (i * PHYS_PAGE_SIZE), NULL);
    }

    TTYFB_back = NULL;
    TTYFB_back_uses_kmem = false;
    TTYFB_back_page_count = 0;
}

static uint8_t* TTYFB_alloc_backbuffer(size_t size, bool* uses_kmem_out)
{
    if (uses_kmem_out)
        *uses_kmem_out = false;
    if (size == 0)
        return NULL;

    uint8_t* kmem_buf = (uint8_t*) kmalloc(size);
    if (kmem_buf)
    {
        if (uses_kmem_out)
            *uses_kmem_out = true;
        return kmem_buf;
    }

    uint64_t max_size = TTYFB_BACKBUFFER_VIRT_SIZE;
    if ((uint64_t) size > max_size)
        return NULL;

    size_t page_count = (size + PHYS_PAGE_SIZE - 1) / PHYS_PAGE_SIZE;
    uintptr_t virt = TTYFB_BACKBUFFER_VIRT_BASE;
    for (size_t i = 0; i < page_count; i++)
    {
        uintptr_t phys = (uintptr_t) PMM_alloc_page();
        if (phys == 0)
        {
            for (size_t rollback = 0; rollback < i; rollback++)
                (void) VMM_unmap_page(virt + (rollback * PHYS_PAGE_SIZE), NULL);
            return NULL;
        }

        VMM_map_page_flags(virt + (i * PHYS_PAGE_SIZE), phys, NO_EXECUTE);
    }

    TTYFB_back_page_count = page_count;
    return (uint8_t*) virt;
}

static void TTYFB_recompute_grid(void)
{
    if (TTYFB_ready && TTYFB_font_ready && TTYFB_font_width != 0 && TTYFB_font_height != 0)
    {
        TTYFB_cols = TTYFB_width / TTYFB_font_width;
        TTYFB_rows = TTYFB_height / TTYFB_font_height;
        if (TTYFB_cols == 0)
            TTYFB_cols = 1;
        if (TTYFB_rows == 0)
            TTYFB_rows = 1;
        return;
    }

    TTYFB_cols = VGA_WIDTH;
    TTYFB_rows = VGA_HEIGHT;
}

static uint32_t TTYFB_scale_channel(uint8_t value, uint8_t bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 31)
        bits = 31;

    uint64_t max_value = (1ULL << bits) - 1ULL;
    return (uint32_t) ((((uint64_t) value * max_value) + 127ULL) / 255ULL);
}

static uint32_t TTYFB_encode_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel = 0;

    if (TTYFB_red_pos < 32)
        pixel |= TTYFB_scale_channel(r, TTYFB_red_size) << TTYFB_red_pos;
    if (TTYFB_green_pos < 32)
        pixel |= TTYFB_scale_channel(g, TTYFB_green_size) << TTYFB_green_pos;
    if (TTYFB_blue_pos < 32)
        pixel |= TTYFB_scale_channel(b, TTYFB_blue_size) << TTYFB_blue_pos;

    return pixel;
}

static inline uint8_t* TTYFB_target(void)
{
    if (TTYFB_double_buffer && TTYFB_back)
        return TTYFB_back;
    return TTYFB_front;
}

static void TTYFB_store_pixel(uint8_t* buffer, uint32_t x, uint32_t y, uint32_t pixel)
{
    if (!buffer || x >= TTYFB_width || y >= TTYFB_height)
        return;

    size_t offset = (size_t) y * TTYFB_pitch + ((size_t) x * TTYFB_bytes_per_pixel);
    uint8_t* dst = buffer + offset;

    switch (TTYFB_bytes_per_pixel)
    {
        case 4:
            *(uint32_t*) dst = pixel;
            break;
        case 3:
            dst[0] = (uint8_t) (pixel & 0xFFU);
            dst[1] = (uint8_t) ((pixel >> 8) & 0xFFU);
            dst[2] = (uint8_t) ((pixel >> 16) & 0xFFU);
            break;
        case 2:
            *(uint16_t*) dst = (uint16_t) (pixel & 0xFFFFU);
            break;
        case 1:
            dst[0] = (uint8_t) (pixel & 0xFFU);
            break;
    }
}

static void TTYFB_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!TTYFB_ready || !TTYFB_double_buffer || !TTYFB_front || !TTYFB_back)
        return;
    if (width == 0 || height == 0)
        return;
    if (x >= TTYFB_width || y >= TTYFB_height)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTYFB_min_u32(TTYFB_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTYFB_min_u32(TTYFB_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    size_t row_offset = (size_t) x * TTYFB_bytes_per_pixel;
    size_t row_length = (size_t) (x2 - x) * TTYFB_bytes_per_pixel;
    for (uint32_t py = y; py < y2; py++)
    {
        uint8_t* src = TTYFB_back + ((size_t) py * TTYFB_pitch) + row_offset;
        uint8_t* dst = TTYFB_front + ((size_t) py * TTYFB_pitch) + row_offset;
        memcpy(dst, src, row_length);
    }
}

static void TTYFB_flush_all(void)
{
    if (!TTYFB_ready || !TTYFB_double_buffer || !TTYFB_front || !TTYFB_back || TTYFB_size == 0)
        return;

    memcpy(TTYFB_front, TTYFB_back, TTYFB_size);
}

static uint32_t TTYFB_color_to_pixel(uint8_t color_index)
{
    color_index &= 0x0F;
    const uint8_t* rgb = TTYFB_vga_palette[color_index];
    return TTYFB_encode_rgb(rgb[0], rgb[1], rgb[2]);
}

static void TTYFB_fill_rect_target(uint8_t* buffer,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t pixel)
{
    if (!buffer || !TTYFB_ready || width == 0 || height == 0)
        return;
    if (x >= TTYFB_width || y >= TTYFB_height)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTYFB_min_u32(TTYFB_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTYFB_min_u32(TTYFB_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    for (uint32_t py = y; py < y2; py++)
    {
        for (uint32_t px = x; px < x2; px++)
            TTYFB_store_pixel(buffer, px, py, pixel);
    }
}

static void TTYFB_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t pixel)
{
    if (!TTYFB_ready || width == 0 || height == 0)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTYFB_min_u32(TTYFB_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTYFB_min_u32(TTYFB_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    TTYFB_fill_rect_target(TTYFB_target(), x, y, width, height, pixel);
    TTYFB_flush_rect(x, y, x2 - x, y2 - y);
}

static void TTYFB_memmove_bytes(uint8_t* dest, const uint8_t* src, size_t len)
{
    if (!dest || !src || len == 0 || dest == src)
        return;

    if (dest < src)
    {
        for (size_t i = 0; i < len; i++)
            dest[i] = src[i];
        return;
    }

    while (len != 0)
    {
        len--;
        dest[len] = src[len];
    }
}

static uint32_t TTYFB_load_pixel(const uint8_t* buffer, uint32_t x, uint32_t y)
{
    if (!buffer || x >= TTYFB_width || y >= TTYFB_height)
        return 0;

    size_t offset = (size_t) y * TTYFB_pitch + ((size_t) x * TTYFB_bytes_per_pixel);
    const uint8_t* src = buffer + offset;

    switch (TTYFB_bytes_per_pixel)
    {
        case 4:
            return *(const uint32_t*) src;
        case 3:
            return (uint32_t) src[0] |
                   ((uint32_t) src[1] << 8) |
                   ((uint32_t) src[2] << 16);
        case 2:
            return *(const uint16_t*) src;
        case 1:
            return src[0];
        default:
            return 0;
    }
}

static uint32_t TTYFB_pixel_invert_mask(void)
{
    switch (TTYFB_bytes_per_pixel)
    {
        case 4:
            return 0x00FFFFFFU;
        case 3:
            return 0x00FFFFFFU;
        case 2:
            return 0x0000FFFFU;
        case 1:
            return 0x000000FFU;
        default:
            return 0;
    }
}

static bool TTYFB_cursor_rect(uint32_t* out_x,
                              uint32_t* out_y,
                              uint32_t* out_width,
                              uint32_t* out_height)
{
    if (!TTYFB_ready || !TTYFB_font_ready || TTYFB_font_width == 0 || TTYFB_font_height == 0)
        return false;
    if (TTYFB_cols == 0 || TTYFB_rows == 0)
        return false;

    uint32_t cell_x = TTYFB_cursor_cell_x;
    uint32_t cell_y = TTYFB_cursor_cell_y;
    if (cell_x >= TTYFB_cols)
        cell_x = TTYFB_cols - 1;
    if (cell_y >= TTYFB_rows)
        cell_y = TTYFB_rows - 1;

    uint32_t cursor_h = (TTYFB_font_height >= TTYFB_CURSOR_THICKNESS_PX) ? TTYFB_CURSOR_THICKNESS_PX : 1U;
    uint32_t x = cell_x * TTYFB_font_width;
    uint32_t y = (cell_y * TTYFB_font_height) + (TTYFB_font_height - cursor_h);
    if (x >= TTYFB_width || y >= TTYFB_height)
        return false;

    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_width)
        *out_width = TTYFB_font_width;
    if (out_height)
        *out_height = cursor_h;
    return true;
}

static void TTYFB_invert_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!TTYFB_ready || width == 0 || height == 0)
        return;

    uint8_t* target = TTYFB_target();
    if (!target)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTYFB_min_u32(TTYFB_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTYFB_min_u32(TTYFB_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    uint32_t mask = TTYFB_pixel_invert_mask();
    if (mask == 0)
        return;

    for (uint32_t py = y; py < y2; py++)
    {
        for (uint32_t px = x; px < x2; px++)
        {
            uint32_t pixel = TTYFB_load_pixel(target, px, py);
            TTYFB_store_pixel(target, px, py, pixel ^ mask);
        }
    }

    TTYFB_flush_rect(x, y, x2 - x, y2 - y);
}

static void TTYFB_cursor_hide(void)
{
    if (!TTYFB_cursor_drawn)
        return;

    uint32_t x = 0, y = 0, width = 0, height = 0;
    if (!TTYFB_cursor_rect(&x, &y, &width, &height))
    {
        TTYFB_cursor_drawn = false;
        return;
    }

    TTYFB_invert_rect(x, y, width, height);
    TTYFB_cursor_drawn = false;
}

static void TTYFB_cursor_show(void)
{
    if (!TTYFB_cursor_enabled || TTYFB_cursor_drawn)
        return;

    uint32_t x = 0, y = 0, width = 0, height = 0;
    if (!TTYFB_cursor_rect(&x, &y, &width, &height))
        return;

    TTYFB_invert_rect(x, y, width, height);
    TTYFB_cursor_drawn = true;
}

static void TTYFB_draw_glyph(char c, uint8_t color, size_t cell_x, size_t cell_y)
{
    if (!TTYFB_ready || !TTYFB_font_ready || !TTYFB_font_bitmap)
        return;
    if (cell_x >= TTYFB_cols || cell_y >= TTYFB_rows)
        return;

    uint32_t glyph_index = (uint8_t) c;
    if (glyph_index >= TTYFB_font_num_glyph)
        glyph_index = (TTYFB_font_num_glyph > (uint32_t) '?') ? (uint32_t) '?' : 0;

    size_t glyph_offset = (size_t) glyph_index * TTYFB_font_bytes_per_glyph;
    if (glyph_offset >= TTYFB_font_bitmap_size)
        return;

    const uint8_t* glyph = TTYFB_font_bitmap + glyph_offset;
    uint32_t start_x = (uint32_t) cell_x * TTYFB_font_width;
    uint32_t start_y = (uint32_t) cell_y * TTYFB_font_height;

    uint32_t fg_pixel = TTYFB_color_to_pixel(color & 0x0F);
    uint32_t bg_pixel = TTYFB_color_to_pixel((color >> 4) & 0x0F);

    uint8_t* target = TTYFB_target();
    for (uint32_t gy = 0; gy < TTYFB_font_height; gy++)
    {
        size_t row_offset = (size_t) gy * TTYFB_font_row_bytes;
        if (row_offset >= TTYFB_font_bytes_per_glyph)
            break;

        const uint8_t* row = glyph + row_offset;
        for (uint32_t gx = 0; gx < TTYFB_font_width; gx++)
        {
            uint8_t bits = row[gx >> 3];
            bool set = (bits & (uint8_t) (0x80U >> (gx & 7U))) != 0;
            TTYFB_store_pixel(target, start_x + gx, start_y + gy, set ? fg_pixel : bg_pixel);
        }
    }

    TTYFB_flush_rect(start_x, start_y, TTYFB_font_width, TTYFB_font_height);
}

bool TTYFB_load_psf2(const uint8_t* data, size_t size)
{
    if (!data || size < sizeof(TTY_psf2_header_t))
        return false;

    TTY_psf2_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != TTY_PSF2_MAGIC)
        return false;
    if (hdr.header_size < sizeof(TTY_psf2_header_t))
        return false;
    if (hdr.width == 0 || hdr.height == 0 || hdr.num_glyph == 0 || hdr.bytes_per_glyph == 0)
        return false;

    uint32_t row_bytes = (hdr.width + 7U) / 8U;
    uint64_t min_bytes_per_glyph = (uint64_t) row_bytes * (uint64_t) hdr.height;
    if ((uint64_t) hdr.bytes_per_glyph < min_bytes_per_glyph)
        return false;

    uint64_t glyph_total = (uint64_t) hdr.num_glyph * (uint64_t) hdr.bytes_per_glyph;
    uint64_t required_size = (uint64_t) hdr.header_size + glyph_total;
    if (required_size > (uint64_t) size)
        return false;
    if (glyph_total == 0 || glyph_total > (uint64_t) ((size_t) -1))
        return false;

    uint8_t* new_bitmap = (uint8_t*) kmalloc((size_t) glyph_total);
    if (!new_bitmap)
        return false;

    memcpy(new_bitmap, data + hdr.header_size, (size_t) glyph_total);

    if (TTYFB_font_bitmap)
        kfree(TTYFB_font_bitmap);

    TTYFB_font_bitmap = new_bitmap;
    TTYFB_font_bitmap_size = (size_t) glyph_total;
    TTYFB_font_ready = true;
    TTYFB_font_width = hdr.width;
    TTYFB_font_height = hdr.height;
    TTYFB_font_num_glyph = hdr.num_glyph;
    TTYFB_font_bytes_per_glyph = hdr.bytes_per_glyph;
    TTYFB_font_row_bytes = row_bytes;
    TTYFB_cursor_drawn = false;
    TTYFB_cursor_blink_ticks = 0;

    TTYFB_recompute_grid();
    if (TTYFB_ready)
        TTYFB_clear(0x0A);

    kdebug_printf("[TTY] PSF2 loaded glyphs=%u glyph_size=%u font=%ux%u\n",
                  (unsigned int) TTYFB_font_num_glyph,
                  (unsigned int) TTYFB_font_bytes_per_glyph,
                  (unsigned int) TTYFB_font_width,
                  (unsigned int) TTYFB_font_height);
    if (TTYFB_ready)
    {
        kdebug_printf("[TTY] text grid cols=%u rows=%u\n",
                      (unsigned int) TTYFB_cols,
                      (unsigned int) TTYFB_rows);
    }

    return true;
}

bool TTYFB_init(const TTY_framebuffer_info_t* info, uint8_t color)
{
    if (!info)
        return false;
    if (!TTYFB_font_ready)
    {
        kdebug_puts("[TTY] framebuffer deferred: PSF2 font not loaded\n");
        return false;
    }
    if (info->type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
    {
        kdebug_printf("[TTY] framebuffer type=%u unsupported (need RGB)\n", (unsigned int) info->type);
        return false;
    }
    if (info->width == 0 || info->height == 0 || info->pitch == 0)
        return false;

    uint8_t bytes_per_pixel = (uint8_t) ((info->bpp + 7U) / 8U);
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4)
        return false;
    if (info->pitch < (uint32_t) ((uint64_t) info->width * bytes_per_pixel))
        return false;

    uint64_t fb_size64 = (uint64_t) info->pitch * (uint64_t) info->height;
    if (fb_size64 == 0 || fb_size64 > (uint64_t) ((size_t) -1))
        return false;

    uintptr_t phys_addr = (uintptr_t) info->phys_addr;
    uintptr_t phys_base = phys_addr & ~(uintptr_t) 0xFFFULL;
    uintptr_t phys_off = phys_addr - phys_base;

    uint64_t map_len64 = (uint64_t) phys_off + fb_size64;
    if (map_len64 == 0 || map_len64 > (uint64_t) ((size_t) -1))
        return false;

    size_t map_len = (size_t) map_len64;
    uintptr_t virt_base = VMM_MMIO_VIRT(phys_base);
    VMM_map_mmio_uc_pages(virt_base, phys_base, map_len);

    TTYFB_release_backbuffer();
    TTYFB_double_buffer = false;

    TTYFB_ready = true;
    TTYFB_phys_addr = phys_addr;
    TTYFB_virt_addr = virt_base + phys_off;
    TTYFB_front = (uint8_t*) TTYFB_virt_addr;
    TTYFB_size = (size_t) fb_size64;
    TTYFB_width = info->width;
    TTYFB_height = info->height;
    TTYFB_pitch = info->pitch;
    TTYFB_bpp = info->bpp;
    TTYFB_bytes_per_pixel = bytes_per_pixel;
    TTYFB_red_pos = info->red_field_position;
    TTYFB_red_size = info->red_mask_size;
    TTYFB_green_pos = info->green_field_position;
    TTYFB_green_size = info->green_mask_size;
    TTYFB_blue_pos = info->blue_field_position;
    TTYFB_blue_size = info->blue_mask_size;

    if (TTYFB_red_size == 0 || TTYFB_green_size == 0 || TTYFB_blue_size == 0)
    {
        if (TTYFB_bpp >= 24)
        {
            TTYFB_red_pos = 16;
            TTYFB_red_size = 8;
            TTYFB_green_pos = 8;
            TTYFB_green_size = 8;
            TTYFB_blue_pos = 0;
            TTYFB_blue_size = 8;
        }
        else
        {
            TTYFB_red_pos = 11;
            TTYFB_red_size = 5;
            TTYFB_green_pos = 5;
            TTYFB_green_size = 6;
            TTYFB_blue_pos = 0;
            TTYFB_blue_size = 5;
        }
    }

    bool back_uses_kmem = false;
    TTYFB_back = TTYFB_alloc_backbuffer(TTYFB_size, &back_uses_kmem);
    if (!TTYFB_back)
    {
        // Back buffer is optional: keep direct rendering to front buffer.
        TTYFB_double_buffer = false;
        kdebug_puts("[TTY] framebuffer: double-buffer allocation failed, using single-buffer mode\n");
    }
    else
    {
        TTYFB_back_uses_kmem = back_uses_kmem;
        memcpy(TTYFB_back, TTYFB_front, TTYFB_size);
        TTYFB_double_buffer = true;
    }

    TTYFB_recompute_grid();
    TTYFB_cursor_enabled = false;
    TTYFB_cursor_drawn = false;
    TTYFB_cursor_cell_x = 0;
    TTYFB_cursor_cell_y = 0;
    TTYFB_cursor_blink_ticks = 0;
    TTYFB_clear(color);

    kdebug_printf("[TTY] framebuffer ready phys=0x%llX virt=0x%llX %ux%u pitch=%u bpp=%u\n",
                  (unsigned long long) TTYFB_phys_addr,
                  (unsigned long long) TTYFB_virt_addr,
                  (unsigned int) TTYFB_width,
                  (unsigned int) TTYFB_height,
                  (unsigned int) TTYFB_pitch,
                  (unsigned int) TTYFB_bpp);
    if (TTYFB_double_buffer)
    {
        if (TTYFB_back_uses_kmem)
        {
            kdebug_printf("[TTY] double-buffer enabled bytes=%llu alloc=kmalloc\n",
                          (unsigned long long) TTYFB_size);
        }
        else
        {
            kdebug_printf("[TTY] double-buffer enabled bytes=%llu alloc=paged pages=%llu virt=0x%llX\n",
                          (unsigned long long) TTYFB_size,
                          (unsigned long long) TTYFB_back_page_count,
                          (unsigned long long) TTYFB_BACKBUFFER_VIRT_BASE);
        }
    }
    else
    {
        kdebug_puts("[TTY] double-buffer disabled (single-buffer mode)\n");
    }
    kdebug_printf("[TTY] text grid cols=%u rows=%u font=%ux%u\n",
                  (unsigned int) TTYFB_cols,
                  (unsigned int) TTYFB_rows,
                  (unsigned int) TTYFB_font_width,
                  (unsigned int) TTYFB_font_height);

    return true;
}

bool TTYFB_is_ready(void)
{
    return TTYFB_ready;
}

void TTYFB_clear(uint8_t color)
{
    if (!TTYFB_ready)
        return;

    TTYFB_cursor_hide();
    uint32_t bg_pixel = TTYFB_color_to_pixel((color >> 4) & 0x0F);
    TTYFB_fill_rect(0, 0, TTYFB_width, TTYFB_height, bg_pixel);
    TTYFB_flush_all();
    TTYFB_cursor_drawn = false;
    TTYFB_cursor_blink_ticks = 0;
}

void TTYFB_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    if (TTYFB_cursor_drawn &&
        x == (size_t) TTYFB_cursor_cell_x &&
        y == (size_t) TTYFB_cursor_cell_y)
    {
        TTYFB_cursor_hide();
    }

    TTYFB_draw_glyph(c, color, x, y);
}

void TTYFB_scroll_one_line(uint8_t color)
{
    TTYFB_cursor_hide();

    if (!TTYFB_ready || !TTYFB_font_ready || TTYFB_font_height == 0 || TTYFB_rows == 0)
    {
        TTYFB_clear(color);
        return;
    }

    uint32_t text_height = TTYFB_rows * TTYFB_font_height;
    if (text_height == 0 || text_height > TTYFB_height)
        text_height = TTYFB_height;

    uint32_t scroll_px = TTYFB_font_height;
    if (scroll_px >= text_height)
    {
        TTYFB_clear(color);
        return;
    }

    uint8_t* target = TTYFB_target();
    if (!target)
    {
        TTYFB_clear(color);
        return;
    }

    size_t src_off = (size_t) scroll_px * TTYFB_pitch;
    size_t move_len = (size_t) (text_height - scroll_px) * TTYFB_pitch;
    TTYFB_memmove_bytes(target, target + src_off, move_len);

    uint32_t bg_pixel = TTYFB_color_to_pixel((color >> 4) & 0x0F);
    TTYFB_fill_rect_target(target, 0, text_height - scroll_px, TTYFB_width, scroll_px, bg_pixel);
    if (TTYFB_double_buffer)
        TTYFB_flush_all();

    TTYFB_cursor_drawn = false;
}

void TTYFB_enable_cursor(bool enabled)
{
    if (!enabled)
    {
        TTYFB_cursor_hide();
        TTYFB_cursor_enabled = false;
        TTYFB_cursor_blink_ticks = 0;
        return;
    }

    TTYFB_cursor_enabled = true;
    TTYFB_cursor_blink_ticks = 0;
    TTYFB_cursor_show();
}

void TTYFB_update_cursor(uint8_t x, uint8_t y)
{
    TTYFB_cursor_hide();

    if (TTYFB_cols != 0)
    {
        if ((uint32_t) x >= TTYFB_cols)
            x = (uint8_t) (TTYFB_cols - 1U);
    }
    else
    {
        x = 0;
    }

    if (TTYFB_rows != 0)
    {
        if ((uint32_t) y >= TTYFB_rows)
            y = (uint8_t) (TTYFB_rows - 1U);
    }
    else
    {
        y = 0;
    }

    TTYFB_cursor_cell_x = x;
    TTYFB_cursor_cell_y = y;
    TTYFB_cursor_blink_ticks = 0;
    TTYFB_cursor_show();
}

void TTYFB_on_timer_tick(void)
{
    if (!TTYFB_cursor_enabled)
        return;
    if (!TTYFB_ready || !TTYFB_font_ready)
        return;

    TTYFB_cursor_blink_ticks++;
    if (TTYFB_cursor_blink_ticks < TTYFB_CURSOR_BLINK_PERIOD_TICKS)
        return;

    TTYFB_cursor_blink_ticks = 0;
    if (TTYFB_cursor_drawn)
        TTYFB_cursor_hide();
    else
        TTYFB_cursor_show();
}

void TTYFB_get_grid(uint32_t* cols, uint32_t* rows)
{
    if (cols)
        *cols = TTYFB_cols;
    if (rows)
        *rows = TTYFB_rows;
}
