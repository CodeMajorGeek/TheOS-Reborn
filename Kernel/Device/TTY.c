#include <Device/TTY.h>

#include <Multiboot2/multiboot2.h>
#include <Memory/KMem.h>
#include <Memory/VMM.h>
#include <Debug/KDebug.h>

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

static uint32_t TTY_row;
static uint32_t TTY_col;
static uint8_t TTY_color;
static bool TTY_cursor_enabled;

static bool TTY_fb_ready;
static uintptr_t TTY_fb_phys_addr;
static uintptr_t TTY_fb_virt_addr;
static uint8_t* TTY_fb_front;
static uint8_t* TTY_fb_back;
static size_t TTY_fb_size;
static bool TTY_fb_double_buffer;
static uint32_t TTY_fb_width;
static uint32_t TTY_fb_height;
static uint32_t TTY_fb_pitch;
static uint8_t TTY_fb_bpp;
static uint8_t TTY_fb_bytes_per_pixel;
static uint8_t TTY_fb_red_pos;
static uint8_t TTY_fb_red_size;
static uint8_t TTY_fb_green_pos;
static uint8_t TTY_fb_green_size;
static uint8_t TTY_fb_blue_pos;
static uint8_t TTY_fb_blue_size;

static bool TTY_font_ready;
static uint8_t* TTY_font_bitmap;
static size_t TTY_font_bitmap_size;
static uint32_t TTY_font_width;
static uint32_t TTY_font_height;
static uint32_t TTY_font_num_glyph;
static uint32_t TTY_font_bytes_per_glyph;
static uint32_t TTY_font_row_bytes;

static uint32_t TTY_cols;
static uint32_t TTY_rows;

static const uint8_t TTY_vga_palette[16][3] =
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

static uint32_t TTY_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static void TTY_store_pixel(uint8_t* buffer, uint32_t x, uint32_t y, uint32_t pixel);

static void TTY_recompute_text_grid(void)
{
    if (TTY_fb_ready && TTY_font_ready && TTY_font_width != 0 && TTY_font_height != 0)
    {
        TTY_cols = TTY_fb_width / TTY_font_width;
        TTY_rows = TTY_fb_height / TTY_font_height;
        if (TTY_cols == 0)
            TTY_cols = 1;
        if (TTY_rows == 0)
            TTY_rows = 1;
        return;
    }

    TTY_cols = 80;
    TTY_rows = 25;
}

static uint32_t TTY_scale_channel(uint8_t value, uint8_t bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 31)
        bits = 31;

    uint64_t max_value = (1ULL << bits) - 1ULL;
    return (uint32_t) ((((uint64_t) value * max_value) + 127ULL) / 255ULL);
}

static uint32_t TTY_encode_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pixel = 0;

    if (TTY_fb_red_pos < 32)
        pixel |= TTY_scale_channel(r, TTY_fb_red_size) << TTY_fb_red_pos;
    if (TTY_fb_green_pos < 32)
        pixel |= TTY_scale_channel(g, TTY_fb_green_size) << TTY_fb_green_pos;
    if (TTY_fb_blue_pos < 32)
        pixel |= TTY_scale_channel(b, TTY_fb_blue_size) << TTY_fb_blue_pos;

    return pixel;
}

static inline uint8_t* TTY_frame_target(void)
{
    if (TTY_fb_double_buffer && TTY_fb_back)
        return TTY_fb_back;
    return TTY_fb_front;
}

static void TTY_memmove_bytes(uint8_t* dest, const uint8_t* src, size_t len)
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

static void TTY_fill_rect_target(uint8_t* buffer,
                                 uint32_t x,
                                 uint32_t y,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t pixel)
{
    if (!buffer || !TTY_fb_ready || width == 0 || height == 0)
        return;
    if (x >= TTY_fb_width || y >= TTY_fb_height)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTY_min_u32(TTY_fb_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTY_min_u32(TTY_fb_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    for (uint32_t py = y; py < y2; py++)
    {
        for (uint32_t px = x; px < x2; px++)
            TTY_store_pixel(buffer, px, py, pixel);
    }
}

static void TTY_store_pixel(uint8_t* buffer, uint32_t x, uint32_t y, uint32_t pixel)
{
    if (!buffer || x >= TTY_fb_width || y >= TTY_fb_height)
        return;

    size_t offset = (size_t) y * TTY_fb_pitch + ((size_t) x * TTY_fb_bytes_per_pixel);
    uint8_t* dst = buffer + offset;

    switch (TTY_fb_bytes_per_pixel)
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

static void TTY_draw_pixel(uint32_t x, uint32_t y, uint32_t pixel)
{
    TTY_store_pixel(TTY_frame_target(), x, y, pixel);
}

static void TTY_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!TTY_fb_ready || !TTY_fb_double_buffer || !TTY_fb_front || !TTY_fb_back)
        return;
    if (width == 0 || height == 0)
        return;
    if (x >= TTY_fb_width || y >= TTY_fb_height)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTY_min_u32(TTY_fb_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTY_min_u32(TTY_fb_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    size_t row_offset = (size_t) x * TTY_fb_bytes_per_pixel;
    size_t row_length = (size_t) (x2 - x) * TTY_fb_bytes_per_pixel;
    for (uint32_t py = y; py < y2; py++)
    {
        uint8_t* src = TTY_fb_back + ((size_t) py * TTY_fb_pitch) + row_offset;
        uint8_t* dst = TTY_fb_front + ((size_t) py * TTY_fb_pitch) + row_offset;
        memcpy(dst, src, row_length);
    }
}

static void TTY_flush_all(void)
{
    if (!TTY_fb_ready || !TTY_fb_double_buffer || !TTY_fb_front || !TTY_fb_back || TTY_fb_size == 0)
        return;
    memcpy(TTY_fb_front, TTY_fb_back, TTY_fb_size);
}

static uint32_t TTY_color_to_pixel(uint8_t color_index)
{
    color_index &= 0x0F;
    const uint8_t* rgb = TTY_vga_palette[color_index];
    return TTY_encode_rgb(rgb[0], rgb[1], rgb[2]);
}

static void TTY_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t pixel)
{
    if (!TTY_fb_ready || width == 0 || height == 0)
        return;

    uint64_t x_end64 = (uint64_t) x + (uint64_t) width;
    uint64_t y_end64 = (uint64_t) y + (uint64_t) height;
    uint32_t x2 = TTY_min_u32(TTY_fb_width, (x_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) x_end64);
    uint32_t y2 = TTY_min_u32(TTY_fb_height, (y_end64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) y_end64);
    if (x2 <= x || y2 <= y)
        return;

    TTY_fill_rect_target(TTY_frame_target(), x, y, width, height, pixel);
    TTY_flush_rect(x, y, x2 - x, y2 - y);
}

static void TTY_scroll_one_line(void)
{
    if (!TTY_fb_ready || !TTY_font_ready || TTY_font_height == 0 || TTY_rows == 0)
    {
        TTY_clear();
        return;
    }

    uint32_t text_height = TTY_rows * TTY_font_height;
    if (text_height == 0 || text_height > TTY_fb_height)
        text_height = TTY_fb_height;

    uint32_t scroll_px = TTY_font_height;
    if (scroll_px >= text_height)
    {
        TTY_clear();
        return;
    }

    uint8_t* target = TTY_frame_target();
    if (!target)
    {
        TTY_clear();
        return;
    }

    size_t src_off = (size_t) scroll_px * TTY_fb_pitch;
    size_t move_len = (size_t) (text_height - scroll_px) * TTY_fb_pitch;
    TTY_memmove_bytes(target, target + src_off, move_len);

    uint32_t bg_pixel = TTY_color_to_pixel((TTY_color >> 4) & 0x0F);
    TTY_fill_rect_target(target, 0, text_height - scroll_px, TTY_fb_width, scroll_px, bg_pixel);

    if (TTY_fb_double_buffer)
        TTY_flush_all();
}

static void TTY_draw_glyph(char c, uint8_t color, size_t cell_x, size_t cell_y)
{
    if (!TTY_fb_ready || !TTY_font_ready || !TTY_font_bitmap)
        return;
    if (cell_x >= TTY_cols || cell_y >= TTY_rows)
        return;

    uint32_t glyph_index = (uint8_t) c;
    if (glyph_index >= TTY_font_num_glyph)
        glyph_index = (TTY_font_num_glyph > (uint32_t) '?') ? (uint32_t) '?' : 0;

    size_t glyph_offset = (size_t) glyph_index * TTY_font_bytes_per_glyph;
    if (glyph_offset >= TTY_font_bitmap_size)
        return;

    const uint8_t* glyph = TTY_font_bitmap + glyph_offset;

    uint32_t start_x = (uint32_t) cell_x * TTY_font_width;
    uint32_t start_y = (uint32_t) cell_y * TTY_font_height;

    uint32_t fg_pixel = TTY_color_to_pixel(color & 0x0F);
    uint32_t bg_pixel = TTY_color_to_pixel((color >> 4) & 0x0F);

    for (uint32_t gy = 0; gy < TTY_font_height; gy++)
    {
        size_t row_offset = (size_t) gy * TTY_font_row_bytes;
        if (row_offset >= TTY_font_bytes_per_glyph)
            break;

        const uint8_t* row = glyph + row_offset;
        for (uint32_t gx = 0; gx < TTY_font_width; gx++)
        {
            uint8_t bits = row[gx >> 3];
            bool set = (bits & (uint8_t) (0x80U >> (gx & 7U))) != 0;
            TTY_draw_pixel(start_x + gx, start_y + gy, set ? fg_pixel : bg_pixel);
        }
    }

    TTY_flush_rect(start_x, start_y, TTY_font_width, TTY_font_height);
}

void TTY_set_buffer(uint16_t* buffer)
{
    (void) buffer;
}

void TTY_init(void)
{
    TTY_row = 0;
    TTY_col = 0;
    TTY_color = 0x0A; // light green on black
    TTY_cursor_enabled = FALSE;

    TTY_recompute_text_grid();
    TTY_clear();
}

bool TTY_init_framebuffer(const TTY_framebuffer_info_t* info)
{
    if (!info)
        return false;

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

    if (TTY_fb_back)
    {
        kfree(TTY_fb_back);
        TTY_fb_back = NULL;
    }

    TTY_fb_ready = true;
    TTY_fb_phys_addr = phys_addr;
    TTY_fb_virt_addr = virt_base + phys_off;
    TTY_fb_front = (uint8_t*) TTY_fb_virt_addr;
    TTY_fb_size = (size_t) fb_size64;
    TTY_fb_width = info->width;
    TTY_fb_height = info->height;
    TTY_fb_pitch = info->pitch;
    TTY_fb_bpp = info->bpp;
    TTY_fb_bytes_per_pixel = bytes_per_pixel;

    TTY_fb_red_pos = info->red_field_position;
    TTY_fb_red_size = info->red_mask_size;
    TTY_fb_green_pos = info->green_field_position;
    TTY_fb_green_size = info->green_mask_size;
    TTY_fb_blue_pos = info->blue_field_position;
    TTY_fb_blue_size = info->blue_mask_size;

    if (TTY_fb_red_size == 0 || TTY_fb_green_size == 0 || TTY_fb_blue_size == 0)
    {
        if (TTY_fb_bpp >= 24)
        {
            TTY_fb_red_pos = 16;
            TTY_fb_red_size = 8;
            TTY_fb_green_pos = 8;
            TTY_fb_green_size = 8;
            TTY_fb_blue_pos = 0;
            TTY_fb_blue_size = 8;
        }
        else
        {
            TTY_fb_red_pos = 11;
            TTY_fb_red_size = 5;
            TTY_fb_green_pos = 5;
            TTY_fb_green_size = 6;
            TTY_fb_blue_pos = 0;
            TTY_fb_blue_size = 5;
        }
    }

    TTY_fb_back = (uint8_t*) kmalloc(TTY_fb_size);
    if (TTY_fb_back)
    {
        memcpy(TTY_fb_back, TTY_fb_front, TTY_fb_size);
        TTY_fb_double_buffer = true;
    }
    else
    {
        TTY_fb_double_buffer = false;
    }

    TTY_recompute_text_grid();
    TTY_clear();

    kdebug_printf("[TTY] framebuffer ready phys=0x%llX virt=0x%llX %ux%u pitch=%u bpp=%u\n",
                  (unsigned long long) TTY_fb_phys_addr,
                  (unsigned long long) TTY_fb_virt_addr,
                  (unsigned int) TTY_fb_width,
                  (unsigned int) TTY_fb_height,
                  (unsigned int) TTY_fb_pitch,
                  (unsigned int) TTY_fb_bpp);
    kdebug_printf("[TTY] double-buffer %s bytes=%llu\n",
                  TTY_fb_double_buffer ? "enabled" : "disabled",
                  (unsigned long long) TTY_fb_size);
    if (TTY_font_ready)
    {
        kdebug_printf("[TTY] text grid cols=%u rows=%u font=%ux%u\n",
                      (unsigned int) TTY_cols,
                      (unsigned int) TTY_rows,
                      (unsigned int) TTY_font_width,
                      (unsigned int) TTY_font_height);
    }
    else
    {
        kdebug_puts("[TTY] waiting for PSF2 font load\n");
    }

    return true;
}

bool TTY_has_framebuffer(void)
{
    return TTY_fb_ready;
}

bool TTY_load_psf2(const uint8_t* data, size_t size)
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

    if (TTY_font_bitmap)
        kfree(TTY_font_bitmap);

    TTY_font_bitmap = new_bitmap;
    TTY_font_bitmap_size = (size_t) glyph_total;
    TTY_font_ready = true;
    TTY_font_width = hdr.width;
    TTY_font_height = hdr.height;
    TTY_font_num_glyph = hdr.num_glyph;
    TTY_font_bytes_per_glyph = hdr.bytes_per_glyph;
    TTY_font_row_bytes = row_bytes;

    TTY_recompute_text_grid();
    TTY_clear();

    kdebug_printf("[TTY] PSF2 loaded glyphs=%u glyph_size=%u font=%ux%u\n",
                  (unsigned int) TTY_font_num_glyph,
                  (unsigned int) TTY_font_bytes_per_glyph,
                  (unsigned int) TTY_font_width,
                  (unsigned int) TTY_font_height);
    if (TTY_fb_ready)
    {
        kdebug_printf("[TTY] text grid cols=%u rows=%u\n",
                      (unsigned int) TTY_cols,
                      (unsigned int) TTY_rows);
    }

    return true;
}

void TTY_clear(void)
{
    TTY_row = 0;
    TTY_col = 0;

    if (!TTY_fb_ready)
        return;

    uint32_t bg_pixel = TTY_color_to_pixel((TTY_color >> 4) & 0x0F);
    TTY_fill_rect(0, 0, TTY_fb_width, TTY_fb_height, bg_pixel);
    TTY_flush_all();
}

void TTY_set_color(uint8_t color)
{
    TTY_color = color;
}

void TTY_put_entry_at(char c, uint8_t color, size_t x, size_t y)
{
    TTY_draw_glyph(c, color, x, y);
}

void TTY_putc(char c)
{
#ifdef USE_COM2_OUTPUT
    COM_putc(TTY_COM_PORT, c);
#endif

    if (TTY_cols == 0 || TTY_rows == 0)
        TTY_recompute_text_grid();

    switch (c)
    {
        case '\n':
            TTY_col = 0;
            TTY_row++;
            if (TTY_row >= TTY_rows)
            {
                TTY_scroll_one_line();
                TTY_row = TTY_rows - 1;
            }
            return;
        case '\r':
            TTY_col = 0;
            return;
        case '\t':
        {
            uint32_t tab_stop = (TTY_col + 4U) & ~3U;
            while (TTY_col < tab_stop)
            {
                TTY_put_entry_at(' ', TTY_color, TTY_col, TTY_row);
                TTY_col++;
                if (TTY_col >= TTY_cols)
                {
                    TTY_col = 0;
                    TTY_row++;
                    if (TTY_row >= TTY_rows)
                    {
                        TTY_scroll_one_line();
                        TTY_row = TTY_rows - 1;
                    }
                }
            }
            return;
        }
        default:
            TTY_put_entry_at(c, TTY_color, TTY_col, TTY_row);
            break;
    }

    TTY_col++;
    if (TTY_col >= TTY_cols)
    {
        TTY_col = 0;
        TTY_row++;
        if (TTY_row >= TTY_rows)
        {
            TTY_scroll_one_line();
            TTY_row = TTY_rows - 1;
        }
    }
}

void TTY_write(const char* str, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        TTY_putc(str[i]);
}

void TTY_puts(const char* str)
{
    TTY_write(str, strlen(str));
}

void TTY_enable_cursor(bool enabled)
{
    TTY_cursor_enabled = enabled;
}

void TTY_update_cursor(uint8_t x, uint8_t y)
{
    (void) x;
    (void) y;
}
