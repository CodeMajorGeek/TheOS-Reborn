#include <window.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define WS_DEFAULT_WINDOW_WIDTH   320U
#define WS_DEFAULT_WINDOW_HEIGHT  200U
#define WS_MIN_WINDOW_WIDTH       64U
#define WS_MIN_WINDOW_HEIGHT      48U
#define WS_DEFAULT_DESKTOP_COLOR  0x0016212BU
#define WS_DEFAULT_WINDOW_COLOR   0x00E7E7E7U
#define WS_DEFAULT_BORDER_COLOR   0x00202836U
#define WS_DEFAULT_TITLEBAR_COLOR 0x00324C65U
#define WS_TITLEBAR_HEIGHT        20U
#define WS_CLOSE_BUTTON_SIZE      14U
#define WS_CLOSE_BUTTON_MARGIN_X  4U
#define WS_CLOSE_BUTTON_MARGIN_Y  3U

#define WS_CONNECTOR_LIST_CAP 8U
#define WS_MODE_LIST_CAP      64U
#define WS_DUMB_MAX_BYTES     (8192ULL * 4096ULL)
#define WS_TARGET_MODE_WIDTH  1280U
#define WS_TARGET_MODE_HEIGHT 800U
#define WS_PSF2_MAGIC         0x864AB572U
#define WS_FONT_MAX_BYTES     (8U * 1024U * 1024U)

#define WS_FONT_PATH_PRIMARY  "/system/fonts/ter-powerline-v14n.psf"
#define WS_FONT_PATH_FALLBACK "/system/fonts/ter-powerline-v12n.psf"

#ifndef WINDOW_DEBUG
#define WINDOW_DEBUG 0
#endif

#if WINDOW_DEBUG
#define WINDOW_LOG(fmt, ...) printf("[window] " fmt, ##__VA_ARGS__)
#else
#define WINDOW_LOG(fmt, ...) do { } while (0)
#endif

typedef struct __attribute__((packed)) ws_psf2_header
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t num_glyph;
    uint32_t bytes_per_glyph;
    uint32_t height;
    uint32_t width;
} ws_psf2_header_t;

static bool ws_read_exact(int fd, void* out, size_t len)
{
    if (fd < 0 || !out)
        return false;

    uint8_t* dst = (uint8_t*) out;
    size_t total = 0U;
    while (total < len)
    {
        ssize_t rc = read(fd, dst + total, len - total);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (rc == 0)
            return false;
        total += (size_t) rc;
    }

    return true;
}

static bool ws_mul_u64_overflow(uint64_t a, uint64_t b, uint64_t* out)
{
    if (!out)
        return true;

    if (a == 0ULL || b == 0ULL)
    {
        *out = 0ULL;
        return false;
    }

    if (a > (UINT64_MAX / b))
        return true;

    *out = a * b;
    return false;
}

static void ws_release_font(ws_context_t* ctx)
{
    if (!ctx)
        return;

    if (ctx->font_bitmap)
        free(ctx->font_bitmap);
    ctx->font_bitmap = NULL;
    ctx->font_bitmap_size = 0U;
    ctx->font_width = 0U;
    ctx->font_height = 0U;
    ctx->font_num_glyph = 0U;
    ctx->font_bytes_per_glyph = 0U;
    ctx->font_row_bytes = 0U;
    ctx->font_ready = false;
}

static int ws_load_font_from_path(ws_context_t* ctx, const char* path)
{
    if (!ctx || !path)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    ws_psf2_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (!ws_read_exact(fd, &hdr, sizeof(hdr)))
        goto fail;

    if (hdr.magic != WS_PSF2_MAGIC ||
        hdr.header_size < sizeof(ws_psf2_header_t) ||
        hdr.width == 0U ||
        hdr.height == 0U ||
        hdr.num_glyph == 0U ||
        hdr.bytes_per_glyph == 0U)
    {
        errno = EINVAL;
        goto fail;
    }

    uint32_t row_bytes = (hdr.width + 7U) / 8U;
    uint64_t min_bytes_per_glyph = (uint64_t) row_bytes * (uint64_t) hdr.height;
    if ((uint64_t) hdr.bytes_per_glyph < min_bytes_per_glyph)
    {
        errno = EINVAL;
        goto fail;
    }

    uint64_t glyph_bytes = 0ULL;
    if (ws_mul_u64_overflow((uint64_t) hdr.num_glyph, (uint64_t) hdr.bytes_per_glyph, &glyph_bytes))
    {
        errno = EINVAL;
        goto fail;
    }

    uint64_t total_required = glyph_bytes;
    if (UINT64_MAX - total_required < (uint64_t) hdr.header_size)
    {
        errno = EINVAL;
        goto fail;
    }
    total_required += (uint64_t) hdr.header_size;

    if (glyph_bytes == 0ULL || glyph_bytes > WS_FONT_MAX_BYTES || glyph_bytes > (uint64_t) SIZE_MAX)
    {
        errno = EFBIG;
        goto fail;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
        goto fail;
    if (st.st_size < 0 || (uint64_t) st.st_size < total_required)
    {
        errno = EINVAL;
        goto fail;
    }

    if (lseek(fd, (off_t) hdr.header_size, SEEK_SET) < 0)
        goto fail;

    uint8_t* bitmap = (uint8_t*) malloc((size_t) glyph_bytes);
    if (!bitmap)
    {
        errno = ENOMEM;
        goto fail;
    }

    if (!ws_read_exact(fd, bitmap, (size_t) glyph_bytes))
    {
        free(bitmap);
        goto fail;
    }

    (void) close(fd);
    ws_release_font(ctx);
    ctx->font_bitmap = bitmap;
    ctx->font_bitmap_size = (size_t) glyph_bytes;
    ctx->font_width = hdr.width;
    ctx->font_height = hdr.height;
    ctx->font_num_glyph = hdr.num_glyph;
    ctx->font_bytes_per_glyph = hdr.bytes_per_glyph;
    ctx->font_row_bytes = row_bytes;
    ctx->font_ready = true;
    return 0;

fail:
    {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
    }
    return -1;
}

static int ws_load_system_font(ws_context_t* ctx)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    if (ws_load_font_from_path(ctx, WS_FONT_PATH_PRIMARY) == 0)
        return 0;

    if (ws_load_font_from_path(ctx, WS_FONT_PATH_FALLBACK) == 0)
        return 0;

    return -1;
}

static bool ws_mode_fits_dumb_limit(const drm_mode_modeinfo_t* mode)
{
    if (!mode || mode->hdisplay == 0U || mode->vdisplay == 0U)
        return false;

    uint64_t pixels = (uint64_t) mode->hdisplay * (uint64_t) mode->vdisplay;
    uint64_t bytes = pixels * 4ULL;
    if (bytes == 0ULL || bytes > WS_DUMB_MAX_BYTES)
        return false;

    return true;
}

static uint32_t ws_abs_u32_diff(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static int32_t ws_clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static bool ws_point_in_active_clip(const ws_context_t* ctx, int32_t x, int32_t y)
{
    if (!ctx || !ctx->clip_enabled)
        return true;

    return x >= ctx->clip_x0 &&
           y >= ctx->clip_y0 &&
           x < ctx->clip_x1 &&
           y < ctx->clip_y1;
}

static bool ws_clip_rect_against_active_clip(const ws_context_t* ctx,
                                             int32_t* io_x0,
                                             int32_t* io_y0,
                                             int32_t* io_x1,
                                             int32_t* io_y1)
{
    if (!io_x0 || !io_y0 || !io_x1 || !io_y1)
        return false;

    int32_t x0 = *io_x0;
    int32_t y0 = *io_y0;
    int32_t x1 = *io_x1;
    int32_t y1 = *io_y1;

    if (ctx && ctx->clip_enabled)
    {
        if (x0 < ctx->clip_x0)
            x0 = ctx->clip_x0;
        if (y0 < ctx->clip_y0)
            y0 = ctx->clip_y0;
        if (x1 > ctx->clip_x1)
            x1 = ctx->clip_x1;
        if (y1 > ctx->clip_y1)
            y1 = ctx->clip_y1;
    }

    if (x0 >= x1 || y0 >= y1)
        return false;

    *io_x0 = x0;
    *io_y0 = y0;
    *io_x1 = x1;
    *io_y1 = y1;
    return true;
}

static bool ws_window_intersects_active_clip(const ws_context_t* ctx, const ws_window_t* window)
{
    if (!ctx || !window || !ctx->clip_enabled)
        return true;

    int32_t x0 = window->x;
    int32_t y0 = window->y;
    int32_t x1 = x0 + (int32_t) window->width;
    int32_t y1 = y0 + (int32_t) window->height;

    if (x1 <= ctx->clip_x0 || y1 <= ctx->clip_y0)
        return false;
    if (x0 >= ctx->clip_x1 || y0 >= ctx->clip_y1)
        return false;
    return true;
}

static bool ws_clip_render_region(const ws_context_t* ctx,
                                  int32_t x,
                                  int32_t y,
                                  uint32_t width,
                                  uint32_t height,
                                  int32_t* out_x,
                                  int32_t* out_y,
                                  uint32_t* out_w,
                                  uint32_t* out_h)
{
    if (!ctx || !out_x || !out_y || !out_w || !out_h || width == 0U || height == 0U)
        return false;

    int32_t max_w = (int32_t) ctx->mode.hdisplay;
    int32_t max_h = (int32_t) ctx->mode.vdisplay;
    if (max_w <= 0 || max_h <= 0)
        return false;

    int64_t rx0 = (int64_t) x;
    int64_t ry0 = (int64_t) y;
    int64_t rx1 = rx0 + (int64_t) width;
    int64_t ry1 = ry0 + (int64_t) height;
    if (rx1 <= 0 || ry1 <= 0 || rx0 >= max_w || ry0 >= max_h)
        return false;

    if (rx0 < 0)
        rx0 = 0;
    if (ry0 < 0)
        ry0 = 0;
    if (rx1 > max_w)
        rx1 = max_w;
    if (ry1 > max_h)
        ry1 = max_h;
    if (rx0 >= rx1 || ry0 >= ry1)
        return false;

    *out_x = (int32_t) rx0;
    *out_y = (int32_t) ry0;
    *out_w = (uint32_t) (rx1 - rx0);
    *out_h = (uint32_t) (ry1 - ry0);
    return true;
}

static void ws_reset_context(ws_context_t* ctx)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->drm_fd = -1;
    ctx->dmabuf_fd = -1;
    ctx->desktop_color = WS_DEFAULT_DESKTOP_COLOR;
    ctx->cursor_visible = false;
    ctx->cursor_x = 0;
    ctx->cursor_y = 0;
    ctx->cursor_color = 0x00FFFFFFU;
    ctx->clip_enabled = false;
    ctx->clip_x0 = 0;
    ctx->clip_y0 = 0;
    ctx->clip_x1 = 0;
    ctx->clip_y1 = 0;
    ctx->next_window_id = 1U;
}

static ws_window_t* ws_find_window_mut(ws_context_t* ctx, uint32_t window_id, uint32_t* out_index)
{
    if (!ctx || window_id == 0U)
        return NULL;

    for (uint32_t i = 0U; i < ctx->window_count; i++)
    {
        if (ctx->windows[i].id == window_id)
        {
            if (out_index)
                *out_index = i;
            return &ctx->windows[i];
        }
    }

    return NULL;
}

static void ws_copy_string_limit(char* out, size_t out_size, const char* in)
{
    static bool ws_copy_log_once = false;
    if (!out || out_size == 0U)
        return;

    out[0] = '\0';
    if (!in)
        return;

    /* Ne pas utiliser strlen(in) : les chaînes venant d'IPC peuvent remplir
     * le champ sans '\0' final ; strlen dépasserait alors du tampon (cf. #PF dans memcpy). */
    size_t max_copy = out_size - 1U;
    size_t len = 0U;
    while (len < max_copy && in[len] != '\0')
        len++;
    if (!ws_copy_log_once)
    {
        WINDOW_LOG("copy_string out=0x%llx in=0x%llx len=%llu\n",
                   (unsigned long long) (uintptr_t) out,
                   (unsigned long long) (uintptr_t) in,
                   (unsigned long long) len);
        ws_copy_log_once = true;
    }
    memcpy(out, in, len);
    out[len] = '\0';
}

static int ws_pick_resources(ws_context_t* ctx)
{
    if (!ctx || ctx->drm_fd < 0)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t connector_ids[WS_CONNECTOR_LIST_CAP];
    uint32_t crtc_ids[WS_CONNECTOR_LIST_CAP];
    uint32_t plane_ids[WS_CONNECTOR_LIST_CAP];
    memset(connector_ids, 0, sizeof(connector_ids));
    memset(crtc_ids, 0, sizeof(crtc_ids));
    memset(plane_ids, 0, sizeof(plane_ids));

    drm_mode_get_resources_t resources;
    memset(&resources, 0, sizeof(resources));
    resources.connector_id_ptr = (uint64_t) (uintptr_t) connector_ids;
    resources.crtc_id_ptr = (uint64_t) (uintptr_t) crtc_ids;
    resources.plane_id_ptr = (uint64_t) (uintptr_t) plane_ids;
    resources.count_connectors = WS_CONNECTOR_LIST_CAP;
    resources.count_crtcs = WS_CONNECTOR_LIST_CAP;
    resources.count_planes = WS_CONNECTOR_LIST_CAP;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GET_RESOURCES, &resources) < 0)
        return -1;

    if (resources.count_connectors == 0U || resources.count_crtcs == 0U || resources.count_planes == 0U)
    {
        errno = ENODEV;
        return -1;
    }

    ctx->connector_id = connector_ids[0];
    ctx->crtc_id = crtc_ids[0];
    ctx->plane_id = plane_ids[0];
    return 0;
}

static int ws_pick_mode(ws_context_t* ctx)
{
    if (!ctx || ctx->drm_fd < 0 || ctx->connector_id == 0U)
    {
        errno = EINVAL;
        return -1;
    }

    drm_mode_get_connector_t connector;
    memset(&connector, 0, sizeof(connector));
    connector.connector_id = ctx->connector_id;

    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GET_CONNECTOR, &connector) < 0)
        return -1;
    if (connector.count_modes == 0U)
    {
        errno = ENODEV;
        return -1;
    }

    uint32_t mode_count = connector.count_modes;
    if (mode_count > WS_MODE_LIST_CAP)
        mode_count = WS_MODE_LIST_CAP;

    drm_mode_modeinfo_t modes[WS_MODE_LIST_CAP];
    memset(modes, 0, sizeof(modes));

    connector.modes_ptr = (uint64_t) (uintptr_t) modes;
    connector.count_modes = mode_count;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GET_CONNECTOR, &connector) < 0)
        return -1;
    if (connector.count_modes == 0U)
    {
        errno = ENODEV;
        return -1;
    }

    drm_mode_get_crtc_t crtc;
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = ctx->crtc_id;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_GET_CRTC, &crtc) == 0 &&
        crtc.mode_valid != 0U &&
        ws_mode_fits_dumb_limit(&crtc.mode))
    {
        ctx->mode = crtc.mode;
        return 0;
    }

    for (uint32_t i = 0U; i < connector.count_modes && i < mode_count; i++)
    {
        if (!ws_mode_fits_dumb_limit(&modes[i]))
            continue;
        if ((uint32_t) modes[i].hdisplay == WS_TARGET_MODE_WIDTH &&
            (uint32_t) modes[i].vdisplay == WS_TARGET_MODE_HEIGHT)
        {
            ctx->mode = modes[i];
            return 0;
        }
    }

    uint32_t best_under_index = UINT32_MAX;
    uint64_t best_under_pixels = 0ULL;
    for (uint32_t i = 0U; i < connector.count_modes && i < mode_count; i++)
    {
        if (!ws_mode_fits_dumb_limit(&modes[i]))
            continue;
        uint32_t w = (uint32_t) modes[i].hdisplay;
        uint32_t h = (uint32_t) modes[i].vdisplay;
        if (w > WS_TARGET_MODE_WIDTH || h > WS_TARGET_MODE_HEIGHT)
            continue;

        uint64_t pixels = (uint64_t) w * (uint64_t) h;
        if (best_under_index == UINT32_MAX || pixels > best_under_pixels)
        {
            best_under_index = i;
            best_under_pixels = pixels;
        }
    }
    if (best_under_index != UINT32_MAX)
    {
        ctx->mode = modes[best_under_index];
        return 0;
    }

    uint32_t nearest_index = UINT32_MAX;
    uint32_t nearest_score = UINT32_MAX;
    for (uint32_t i = 0U; i < connector.count_modes && i < mode_count; i++)
    {
        if (!ws_mode_fits_dumb_limit(&modes[i]))
            continue;

        uint32_t w = (uint32_t) modes[i].hdisplay;
        uint32_t h = (uint32_t) modes[i].vdisplay;
        uint32_t score = ws_abs_u32_diff(w, WS_TARGET_MODE_WIDTH) +
                         ws_abs_u32_diff(h, WS_TARGET_MODE_HEIGHT);
        if (score < nearest_score)
        {
            nearest_score = score;
            nearest_index = i;
        }
    }
    if (nearest_index != UINT32_MAX)
    {
        ctx->mode = modes[nearest_index];
        return 0;
    }

    ctx->mode = modes[mode_count - 1U];
    return 0;
}

static int ws_setup_buffer(ws_context_t* ctx)
{
    if (!ctx || ctx->drm_fd < 0)
    {
        errno = EINVAL;
        return -1;
    }

    drm_mode_create_dumb_t dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = ctx->mode.hdisplay;
    dumb.height = ctx->mode.vdisplay;
    dumb.bpp = 32U;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0)
        return -1;

    if (dumb.pitch == 0U || dumb.size == 0U)
    {
        drm_mode_destroy_dumb_t destroy;
        destroy.handle = dumb.handle;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        errno = EIO;
        return -1;
    }

    uint64_t min_pitch = (uint64_t) ctx->mode.hdisplay * 4ULL;
    uint64_t min_size = (uint64_t) dumb.pitch * (uint64_t) ctx->mode.vdisplay;
    if ((uint64_t) dumb.pitch < min_pitch || (uint64_t) dumb.size < min_size)
    {
        drm_mode_destroy_dumb_t destroy;
        destroy.handle = dumb.handle;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        errno = EIO;
        return -1;
    }

    drm_prime_handle_t prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = dumb.handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(ctx->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
    {
        drm_mode_destroy_dumb_t destroy;
        destroy.handle = dumb.handle;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return -1;
    }

    void* map = mmap(NULL,
                     (size_t) dumb.size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     prime.fd,
                     0);
    if (map == MAP_FAILED)
    {
        (void) close(prime.fd);
        drm_mode_destroy_dumb_t destroy;
        destroy.handle = dumb.handle;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return -1;
    }

    drm_mode_create_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t) (uintptr_t) &ctx->mode;
    blob.length = sizeof(ctx->mode);
    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_CREATE_BLOB, &blob) < 0)
    {
        (void) munmap(map, (size_t) dumb.size);
        (void) close(prime.fd);
        drm_mode_destroy_dumb_t destroy;
        destroy.handle = dumb.handle;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return -1;
    }

    memset(&ctx->atomic_req, 0, sizeof(ctx->atomic_req));
    ctx->atomic_req.connector_id = ctx->connector_id;
    ctx->atomic_req.crtc_id = ctx->crtc_id;
    ctx->atomic_req.plane_id = ctx->plane_id;
    ctx->atomic_req.fb_handle = dumb.handle;
    ctx->atomic_req.mode_blob_id = blob.blob_id;
    ctx->atomic_req.active = 1U;
    ctx->atomic_req.src_x = 0U;
    ctx->atomic_req.src_y = 0U;
    ctx->atomic_req.src_w = ((uint32_t) ctx->mode.hdisplay) << 16;
    ctx->atomic_req.src_h = ((uint32_t) ctx->mode.vdisplay) << 16;
    ctx->atomic_req.crtc_x = 0;
    ctx->atomic_req.crtc_y = 0;
    ctx->atomic_req.crtc_w = ctx->mode.hdisplay;
    ctx->atomic_req.crtc_h = ctx->mode.vdisplay;

    ctx->dumb_handle = dumb.handle;
    ctx->dumb_pitch = dumb.pitch;
    ctx->dumb_size = dumb.size;
    ctx->dmabuf_fd = prime.fd;
    ctx->frame_map = (uint8_t*) map;
    ctx->mode_blob_id = blob.blob_id;
    return 0;
}

static void ws_put_pixel(ws_context_t* ctx, uint32_t x, uint32_t y, uint32_t color)
{
    if (!ctx || !ctx->frame_map)
        return;
    if (x >= (uint32_t) ctx->mode.hdisplay || y >= (uint32_t) ctx->mode.vdisplay)
        return;
    if (!ws_point_in_active_clip(ctx, (int32_t) x, (int32_t) y))
        return;

    uint8_t* row = ctx->frame_map + ((size_t) y * (size_t) ctx->dumb_pitch);
    if (ctx->dumb_size != 0U)
    {
        size_t row_offset = (size_t) y * (size_t) ctx->dumb_pitch;
        if (row_offset + (size_t) ctx->dumb_pitch > (size_t) ctx->dumb_size)
            return;
    }
    ((uint32_t*) row)[x] = color;
}

static void ws_fill_rect(ws_context_t* ctx,
                         int32_t x,
                         int32_t y,
                         uint32_t width,
                         uint32_t height,
                         uint32_t color);

static uint32_t ws_contrast_color(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFFU;
    uint32_t g = (rgb >> 8) & 0xFFU;
    uint32_t b = rgb & 0xFFU;
    uint32_t luma = (r * 299U) + (g * 587U) + (b * 114U);
    if (luma >= 128000U)
        return 0x00151D27U;
    return 0x00F4F8FCU;
}

static uint32_t ws_glyph_index_for_char(const ws_context_t* ctx, unsigned char ch)
{
    if (!ctx || ctx->font_num_glyph == 0U)
        return 0U;

    uint32_t glyph_index = (uint32_t) ch;
    if (glyph_index < ctx->font_num_glyph)
        return glyph_index;

    if (ctx->font_num_glyph > (uint32_t) '?')
        return (uint32_t) '?';
    return 0U;
}

static void ws_draw_text_psf2(ws_context_t* ctx,
                              int32_t x,
                              int32_t y,
                              uint32_t max_width,
                              uint32_t color,
                              const char* text)
{
    if (!ctx ||
        !text ||
        max_width == 0U ||
        !ctx->font_ready ||
        !ctx->font_bitmap ||
        ctx->font_width == 0U ||
        ctx->font_height == 0U ||
        ctx->font_bytes_per_glyph == 0U ||
        ctx->font_row_bytes == 0U)
        return;

    int32_t max_x = x + (int32_t) max_width;
    int32_t pen_x = x;
    uint32_t advance = ctx->font_width + 1U;
    int32_t vis_x0 = 0;
    int32_t vis_y0 = 0;
    int32_t vis_x1 = (int32_t) ctx->mode.hdisplay;
    int32_t vis_y1 = (int32_t) ctx->mode.vdisplay;

    if (ctx->clip_enabled)
    {
        if (vis_x0 < ctx->clip_x0)
            vis_x0 = ctx->clip_x0;
        if (vis_y0 < ctx->clip_y0)
            vis_y0 = ctx->clip_y0;
        if (vis_x1 > ctx->clip_x1)
            vis_x1 = ctx->clip_x1;
        if (vis_y1 > ctx->clip_y1)
            vis_y1 = ctx->clip_y1;
    }

    if (vis_x0 >= vis_x1 || vis_y0 >= vis_y1)
        return;
    if (y + (int32_t) ctx->font_height <= vis_y0 || y >= vis_y1)
        return;

    for (size_t i = 0; text[i] != '\0'; i++)
    {
        if (pen_x + (int32_t) ctx->font_width > max_x)
            break;

        uint32_t glyph_index = ws_glyph_index_for_char(ctx, (unsigned char) text[i]);
        size_t glyph_offset = (size_t) glyph_index * (size_t) ctx->font_bytes_per_glyph;
        if (glyph_offset >= ctx->font_bitmap_size)
        {
            pen_x += (int32_t) advance;
            continue;
        }

        int32_t glyph_x0 = pen_x;
        int32_t glyph_y0 = y;
        int32_t glyph_x1 = pen_x + (int32_t) ctx->font_width;
        int32_t glyph_y1 = y + (int32_t) ctx->font_height;
        if (glyph_x1 <= vis_x0 || glyph_y1 <= vis_y0 || glyph_x0 >= vis_x1 || glyph_y0 >= vis_y1)
        {
            pen_x += (int32_t) advance;
            continue;
        }

        uint32_t gy_begin = 0U;
        uint32_t gy_end = ctx->font_height;
        if (glyph_y0 < vis_y0)
            gy_begin = (uint32_t) (vis_y0 - glyph_y0);
        if (glyph_y1 > vis_y1)
            gy_end = (uint32_t) (vis_y1 - glyph_y0);

        uint32_t gx_begin = 0U;
        uint32_t gx_end = ctx->font_width;
        if (glyph_x0 < vis_x0)
            gx_begin = (uint32_t) (vis_x0 - glyph_x0);
        if (glyph_x1 > vis_x1)
            gx_end = (uint32_t) (vis_x1 - glyph_x0);

        if (gx_begin >= gx_end || gy_begin >= gy_end)
        {
            pen_x += (int32_t) advance;
            continue;
        }

        const uint8_t* glyph = ctx->font_bitmap + glyph_offset;
        for (uint32_t gy = gy_begin; gy < gy_end; gy++)
        {
            size_t row_offset = (size_t) gy * (size_t) ctx->font_row_bytes;
            if (row_offset >= (size_t) ctx->font_bytes_per_glyph)
                break;

            const uint8_t* row = glyph + row_offset;
            int32_t py = y + (int32_t) gy;
            size_t dst_row_offset = (size_t) py * (size_t) ctx->dumb_pitch;
            if (ctx->dumb_size != 0U &&
                dst_row_offset + (size_t) ctx->dumb_pitch > (size_t) ctx->dumb_size)
                continue;
            uint8_t* dst_row = ctx->frame_map + dst_row_offset;
            uint32_t* dst_pixels = (uint32_t*) dst_row;
            for (uint32_t gx = gx_begin; gx < gx_end; gx++)
            {
                uint8_t mask = (uint8_t) (0x80U >> (gx & 7U));
                if ((row[gx >> 3U] & mask) == 0U)
                    continue;

                int32_t px = pen_x + (int32_t) gx;
                dst_pixels[px] = color;
            }
        }

        pen_x += (int32_t) advance;
    }
}

static void ws_draw_window_body_text(ws_context_t* ctx, const ws_window_t* window)
{
    if (!ctx || !window || window->body_text[0] == '\0')
        return;
    if (!ctx->font_ready || ctx->font_width == 0U || ctx->font_height == 0U)
        return;
    if (window->height <= WS_TITLEBAR_HEIGHT + 6U || window->width <= 12U)
        return;

    uint32_t text_color = ws_contrast_color(window->color);
    int32_t content_x = window->x + 6;
    int32_t content_y = window->y + (int32_t) WS_TITLEBAR_HEIGHT + 4;
    uint32_t content_w = window->width - 12U;
    uint32_t line_h = ctx->font_height + 2U;
    int32_t max_y = window->y + (int32_t) window->height - 3;
    int32_t vis_x0 = 0;
    int32_t vis_y0 = 0;
    int32_t vis_x1 = (int32_t) ctx->mode.hdisplay;
    int32_t vis_y1 = (int32_t) ctx->mode.vdisplay;

    if (ctx->clip_enabled)
    {
        if (vis_x0 < ctx->clip_x0)
            vis_x0 = ctx->clip_x0;
        if (vis_y0 < ctx->clip_y0)
            vis_y0 = ctx->clip_y0;
        if (vis_x1 > ctx->clip_x1)
            vis_x1 = ctx->clip_x1;
        if (vis_y1 > ctx->clip_y1)
            vis_y1 = ctx->clip_y1;
    }
    if (vis_x0 >= vis_x1 || vis_y0 >= vis_y1)
        return;

    const char* src = window->body_text;
    char line_buf[WS_WINDOW_BODY_TEXT_MAX + 1U];
    static bool ws_body_text_log_once = false;
    while (*src != '\0')
    {
        size_t len = 0U;
        while (src[len] != '\0' && src[len] != '\n')
            len++;
        if (len > WS_WINDOW_BODY_TEXT_MAX)
            len = WS_WINDOW_BODY_TEXT_MAX;

        if (!ws_body_text_log_once)
        {
            WINDOW_LOG("body_text src=0x%llx len=%llu\n",
                       (unsigned long long) (uintptr_t) src,
                       (unsigned long long) len);
            ws_body_text_log_once = true;
        }
        memcpy(line_buf, src, len);
        line_buf[len] = '\0';
        if (content_y + (int32_t) ctx->font_height > max_y)
            break;
        if (content_y >= vis_y1)
            break;

        int32_t line_x0 = content_x;
        int32_t line_y0 = content_y;
        int32_t line_x1 = content_x + (int32_t) content_w;
        int32_t line_y1 = content_y + (int32_t) ctx->font_height;
        if (!(line_x1 <= vis_x0 || line_y1 <= vis_y0 || line_x0 >= vis_x1 || line_y0 >= vis_y1))
            ws_draw_text_psf2(ctx, content_x, content_y, content_w, text_color, line_buf);
        content_y += (int32_t) line_h;

        src += len;
        if (*src == '\n')
            src++;
    }
}

static void ws_draw_close_button(ws_context_t* ctx, const ws_window_t* window)
{
    if (!ctx || !window || !window->frame_controls)
        return;
    if (window->width <= (WS_CLOSE_BUTTON_SIZE + (WS_CLOSE_BUTTON_MARGIN_X * 2U)))
        return;

    int32_t bx = window->x + (int32_t) window->width - (int32_t) WS_CLOSE_BUTTON_MARGIN_X - (int32_t) WS_CLOSE_BUTTON_SIZE;
    int32_t by = window->y + (int32_t) WS_CLOSE_BUTTON_MARGIN_Y;
    ws_fill_rect(ctx, bx, by, WS_CLOSE_BUTTON_SIZE, WS_CLOSE_BUTTON_SIZE, 0x00B74F5BU);

    for (uint32_t i = 3U; i + 3U < WS_CLOSE_BUTTON_SIZE; i++)
    {
        ws_put_pixel(ctx, (uint32_t) (bx + (int32_t) i), (uint32_t) (by + (int32_t) i), 0x00F4F8FCU);
        ws_put_pixel(ctx,
                     (uint32_t) (bx + (int32_t) (WS_CLOSE_BUTTON_SIZE - 1U - i)),
                     (uint32_t) (by + (int32_t) i),
                     0x00F4F8FCU);
    }
}

static void ws_draw_cursor(ws_context_t* ctx)
{
    if (!ctx || !ctx->cursor_visible)
        return;

    static const uint16_t cursor_mask[WS_CURSOR_HEIGHT] = {
        0x001U, 0x003U, 0x007U, 0x00FU,
        0x01FU, 0x03FU, 0x07FU, 0x0FFU,
        0x1FFU, 0x3FFU, 0x7FFU, 0xFFFU,
        0x7FEU, 0x3FCU, 0x1F8U, 0x0F0U
    };

    int32_t base_x = ctx->cursor_x;
    int32_t base_y = ctx->cursor_y;

    for (uint32_t row = 0U; row < WS_CURSOR_HEIGHT; row++)
    {
        uint16_t mask = cursor_mask[row];
        for (uint32_t col = 0U; col < WS_CURSOR_WIDTH; col++)
        {
            if ((mask & (1U << col)) == 0U)
                continue;
            ws_put_pixel(ctx,
                         (uint32_t) (base_x + (int32_t) WS_CURSOR_SHADOW_OFFSET + (int32_t) col),
                         (uint32_t) (base_y + (int32_t) WS_CURSOR_SHADOW_OFFSET + (int32_t) row),
                         0x00000000U);
        }
    }

    for (uint32_t row = 0U; row < WS_CURSOR_HEIGHT; row++)
    {
        uint16_t mask = cursor_mask[row];
        for (uint32_t col = 0U; col < WS_CURSOR_WIDTH; col++)
        {
            if ((mask & (1U << col)) == 0U)
                continue;
            ws_put_pixel(ctx, (uint32_t) (base_x + (int32_t) col), (uint32_t) (base_y + (int32_t) row), ctx->cursor_color);
        }
    }
}

static void ws_fill_rect(ws_context_t* ctx,
                         int32_t x,
                         int32_t y,
                         uint32_t width,
                         uint32_t height,
                         uint32_t color)
{
    static bool ws_fill_rect_log_once = false;
    if (!ctx || !ctx->frame_map || width == 0U || height == 0U)
        return;

    int32_t max_w = (int32_t) ctx->mode.hdisplay;
    int32_t max_h = (int32_t) ctx->mode.vdisplay;

    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + (int32_t) width;
    int32_t y1 = y + (int32_t) height;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > max_w)
        x1 = max_w;
    if (y1 > max_h)
        y1 = max_h;
    if (!ws_clip_rect_against_active_clip(ctx, &x0, &y0, &x1, &y1))
        return;

    int32_t span_pixels = x1 - x0;
    size_t span_bytes = (size_t) span_pixels * sizeof(uint32_t);

    uint8_t* first_row = ctx->frame_map + ((size_t) y0 * (size_t) ctx->dumb_pitch);
    if (ctx->dumb_size != 0U)
    {
        size_t row_offset = (size_t) y0 * (size_t) ctx->dumb_pitch;
        if (row_offset + (size_t) ctx->dumb_pitch > (size_t) ctx->dumb_size)
        {
            if (!ws_fill_rect_log_once)
            {
                WINDOW_LOG("fill_rect oob row y0=%d pitch=%u size=%llu map=0x%llx\n",
                           y0,
                           (unsigned int) ctx->dumb_pitch,
                           (unsigned long long) ctx->dumb_size,
                           (unsigned long long) (uintptr_t) ctx->frame_map);
                ws_fill_rect_log_once = true;
            }
            return;
        }
        if (span_bytes > (size_t) ctx->dumb_pitch)
        {
            if (!ws_fill_rect_log_once)
            {
                WINDOW_LOG("fill_rect span_bytes=%llu pitch=%u x0=%d x1=%d\n",
                           (unsigned long long) span_bytes,
                           (unsigned int) ctx->dumb_pitch,
                           x0,
                           x1);
                ws_fill_rect_log_once = true;
            }
            return;
        }
    }
    if (!ws_fill_rect_log_once)
    {
        WINDOW_LOG("fill_rect x0=%d y0=%d x1=%d y1=%d pitch=%u size=%llu map=0x%llx\n",
                   x0,
                   y0,
                   x1,
                   y1,
                   (unsigned int) ctx->dumb_pitch,
                   (unsigned long long) ctx->dumb_size,
                   (unsigned long long) (uintptr_t) ctx->frame_map);
        ws_fill_rect_log_once = true;
    }
    for (int32_t yy = y0; yy < y1; yy++)
    {
        uint8_t* row = ctx->frame_map + ((size_t) yy * (size_t) ctx->dumb_pitch);
        if (ctx->dumb_size != 0U)
        {
            size_t row_offset = (size_t) yy * (size_t) ctx->dumb_pitch;
            if (row_offset + (size_t) ctx->dumb_pitch > (size_t) ctx->dumb_size)
                break;
        }
        uint32_t* dst = ((uint32_t*) row) + x0;
        for (int32_t xx = 0; xx < span_pixels; xx++)
            dst[xx] = color;
    }
}

static void ws_draw_window(ws_context_t* ctx, const ws_window_t* window)
{
    if (!ctx || !window || !window->visible || window->width == 0U || window->height == 0U)
        return;
    if (!ws_window_intersects_active_clip(ctx, window))
        return;

    ws_fill_rect(ctx, window->x, window->y, window->width, window->height, window->color);

    if (window->width >= 2U && window->height >= 2U)
    {
        ws_fill_rect(ctx, window->x, window->y, window->width, 1U, window->border_color);
        ws_fill_rect(ctx, window->x, window->y + (int32_t) window->height - 1, window->width, 1U, window->border_color);
        ws_fill_rect(ctx, window->x, window->y, 1U, window->height, window->border_color);
        ws_fill_rect(ctx, window->x + (int32_t) window->width - 1, window->y, 1U, window->height, window->border_color);
    }

    uint32_t title_h = WS_TITLEBAR_HEIGHT;
    if (title_h > window->height)
        title_h = window->height;
    if (title_h >= 2U && window->width >= 2U)
    {
        ws_fill_rect(ctx,
                     window->x + 1,
                     window->y + 1,
                     window->width - 2U,
                     title_h - 1U,
                     window->titlebar_color);

        bool title_text_visible = true;
        if (ctx->clip_enabled)
        {
            int32_t tx0 = window->x + 1;
            int32_t ty0 = window->y + 1;
            int32_t tx1 = tx0 + (int32_t) window->width - 2;
            int32_t ty1 = ty0 + (int32_t) title_h - 1;
            title_text_visible = !(tx1 <= ctx->clip_x0 ||
                                   ty1 <= ctx->clip_y0 ||
                                   tx0 >= ctx->clip_x1 ||
                                   ty0 >= ctx->clip_y1);
        }
        if (title_text_visible && window->title[0] != '\0' && window->width > 12U && title_h >= 10U)
        {
            uint32_t text_color = ws_contrast_color(window->titlebar_color);
            uint32_t title_max_width = window->width - 10U;
            if (window->frame_controls &&
                title_max_width > (WS_CLOSE_BUTTON_SIZE + WS_CLOSE_BUTTON_MARGIN_X + 6U))
            {
                title_max_width -= (WS_CLOSE_BUTTON_SIZE + WS_CLOSE_BUTTON_MARGIN_X + 6U);
            }

            ws_draw_text_psf2(ctx,
                              window->x + 5,
                              window->y + 3,
                              title_max_width,
                              text_color,
                              window->title);
        }
    }

    ws_draw_close_button(ctx, window);
    ws_draw_window_body_text(ctx, window);
}

static void ws_redraw_scene(ws_context_t* ctx)
{
    if (!ctx || !ctx->ready || !ctx->frame_map)
        return;

    ws_fill_rect(ctx,
                 0,
                 0,
                 (uint32_t) ctx->mode.hdisplay,
                 (uint32_t) ctx->mode.vdisplay,
                 ctx->desktop_color);

    ws_fill_rect(ctx,
                 0,
                 0,
                 (uint32_t) ctx->mode.hdisplay,
                 24U,
                 0x00101922U);

    for (uint32_t i = 0U; i < ctx->window_count; i++)
        ws_draw_window(ctx, &ctx->windows[i]);

    ws_draw_cursor(ctx);
}

int ws_open(ws_context_t* ctx, bool take_master)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    ws_reset_context(ctx);
    ctx->drm_fd = open(DRM_NODE_PATH, O_RDWR);
    if (ctx->drm_fd < 0)
        return -1;

    if (take_master)
    {
        if (ioctl(ctx->drm_fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
            goto fail;
        ctx->master_owned = true;
    }

    if (ws_pick_resources(ctx) < 0)
        goto fail;
    if (ws_pick_mode(ctx) < 0)
        goto fail;
    if (ws_setup_buffer(ctx) < 0)
        goto fail;
    if (ws_load_system_font(ctx) < 0)
        goto fail;

    ctx->cursor_x = (int32_t) ctx->mode.hdisplay / 2;
    ctx->cursor_y = (int32_t) ctx->mode.vdisplay / 2;

    ctx->ready = true;
    return 0;

fail:
    {
        int open_errno = errno;
        ws_close(ctx);
        errno = open_errno;
    }
    return -1;
}

void ws_close(ws_context_t* ctx)
{
    if (!ctx)
        return;

    int saved_errno = errno;

    if (ctx->drm_fd >= 0 && ctx->ready)
    {
        drm_mode_atomic_req_t disable_req;
        disable_req = ctx->atomic_req;
        disable_req.active = 0U;
        disable_req.fb_handle = 0U;
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_ATOMIC, &disable_req);
    }

    if (ctx->drm_fd >= 0 && ctx->mode_blob_id != 0U)
    {
        drm_mode_destroy_blob_t blob = {
            .blob_id = ctx->mode_blob_id
        };
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_BLOB, &blob);
    }
    ctx->mode_blob_id = 0U;

    if (ctx->frame_map && ctx->dumb_size != 0U)
        (void) munmap(ctx->frame_map, (size_t) ctx->dumb_size);
    ctx->frame_map = NULL;

    if (ctx->dmabuf_fd >= 0)
        (void) close(ctx->dmabuf_fd);
    ctx->dmabuf_fd = -1;

    if (ctx->drm_fd >= 0 && ctx->dumb_handle != 0U)
    {
        drm_mode_destroy_dumb_t dumb = {
            .handle = ctx->dumb_handle
        };
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
    }
    ctx->dumb_handle = 0U;
    ctx->dumb_pitch = 0U;
    ctx->dumb_size = 0U;

    if (ctx->drm_fd >= 0 && ctx->master_owned)
        (void) ioctl(ctx->drm_fd, DRM_IOCTL_DROP_MASTER, NULL);
    ctx->master_owned = false;

    if (ctx->drm_fd >= 0)
        (void) close(ctx->drm_fd);

    ws_release_font(ctx);
    ws_reset_context(ctx);
    errno = saved_errno;
}

int ws_set_desktop_color(ws_context_t* ctx, uint32_t color)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    ctx->desktop_color = color;
    return 0;
}

int ws_create_window(ws_context_t* ctx, const ws_window_desc_t* desc, uint32_t* out_window_id)
{
    if (!ctx || !ctx->ready)
    {
        errno = EINVAL;
        return -1;
    }
    if (ctx->window_count >= WS_MAX_WINDOWS)
    {
        errno = ENOSPC;
        return -1;
    }

    ws_window_t window;
    memset(&window, 0, sizeof(window));

    uint32_t width = WS_DEFAULT_WINDOW_WIDTH;
    uint32_t height = WS_DEFAULT_WINDOW_HEIGHT;
    int32_t x = 24 + (int32_t) (ctx->window_count * 20U);
    int32_t y = 24 + (int32_t) (ctx->window_count * 18U);
    uint32_t color = WS_DEFAULT_WINDOW_COLOR;
    uint32_t border_color = WS_DEFAULT_BORDER_COLOR;
    uint32_t titlebar_color = WS_DEFAULT_TITLEBAR_COLOR;
    bool visible = true;
    bool frame_controls = false;
    const char* title = "Window";

    if (desc)
    {
        if (desc->width != 0U)
            width = desc->width;
        if (desc->height != 0U)
            height = desc->height;
        x = desc->x;
        y = desc->y;
        if (desc->color != 0U)
            color = desc->color;
        if (desc->border_color != 0U)
            border_color = desc->border_color;
        if (desc->titlebar_color != 0U)
            titlebar_color = desc->titlebar_color;
        visible = desc->visible;
        frame_controls = desc->frame_controls;
        if (desc->title && desc->title[0] != '\0')
            title = desc->title;
    }

    if (width < WS_MIN_WINDOW_WIDTH)
        width = WS_MIN_WINDOW_WIDTH;
    if (height < WS_MIN_WINDOW_HEIGHT)
        height = WS_MIN_WINDOW_HEIGHT;

    window.id = ctx->next_window_id++;
    if (window.id == 0U)
        window.id = ctx->next_window_id++;
    window.x = x;
    window.y = y;
    window.width = width;
    window.height = height;
    window.color = color;
    window.border_color = border_color;
    window.titlebar_color = titlebar_color;
    window.visible = visible;
    window.frame_controls = frame_controls;
    ws_copy_string_limit(window.title, sizeof(window.title), title);

    ctx->windows[ctx->window_count++] = window;
    if (out_window_id)
        *out_window_id = window.id;
    return 0;
}

int ws_destroy_window(ws_context_t* ctx, uint32_t window_id)
{
    if (!ctx || !ctx->ready || window_id == 0U)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t index = 0U;
    if (!ws_find_window_mut(ctx, window_id, &index))
    {
        errno = ENOENT;
        return -1;
    }

    for (uint32_t i = index; i + 1U < ctx->window_count; i++)
        ctx->windows[i] = ctx->windows[i + 1U];

    ctx->window_count--;
    memset(&ctx->windows[ctx->window_count], 0, sizeof(ctx->windows[ctx->window_count]));
    return 0;
}

int ws_move_window(ws_context_t* ctx, uint32_t window_id, int32_t x, int32_t y)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    window->x = x;
    window->y = y;
    return 0;
}

int ws_raise_window(ws_context_t* ctx, uint32_t window_id)
{
    if (!ctx || !ctx->ready || window_id == 0U)
    {
        errno = EINVAL;
        return -1;
    }

    uint32_t index = 0U;
    ws_window_t* window = ws_find_window_mut(ctx, window_id, &index);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    if (index + 1U >= ctx->window_count)
        return 0;

    ws_window_t moved = *window;
    for (uint32_t i = index; i + 1U < ctx->window_count; i++)
        ctx->windows[i] = ctx->windows[i + 1U];
    ctx->windows[ctx->window_count - 1U] = moved;
    return 0;
}

int ws_resize_window(ws_context_t* ctx, uint32_t window_id, uint32_t width, uint32_t height)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    if (width < WS_MIN_WINDOW_WIDTH)
        width = WS_MIN_WINDOW_WIDTH;
    if (height < WS_MIN_WINDOW_HEIGHT)
        height = WS_MIN_WINDOW_HEIGHT;

    window->width = width;
    window->height = height;
    return 0;
}

int ws_set_window_color(ws_context_t* ctx, uint32_t window_id, uint32_t color)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    window->color = color;
    return 0;
}

int ws_set_window_visible(ws_context_t* ctx, uint32_t window_id, bool visible)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    window->visible = visible;
    return 0;
}

int ws_set_window_title(ws_context_t* ctx, uint32_t window_id, const char* title)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    ws_copy_string_limit(window->title, sizeof(window->title), title);
    return 0;
}

int ws_set_window_text(ws_context_t* ctx, uint32_t window_id, const char* text)
{
    ws_window_t* window = ws_find_window_mut(ctx, window_id, NULL);
    if (!window)
    {
        errno = ENOENT;
        return -1;
    }

    ws_copy_string_limit(window->body_text, sizeof(window->body_text), text);
    return 0;
}

int ws_find_window(const ws_context_t* ctx, uint32_t window_id, ws_window_t* out_window)
{
    if (!ctx || window_id == 0U || !out_window)
    {
        errno = EINVAL;
        return -1;
    }

    for (uint32_t i = 0U; i < ctx->window_count; i++)
    {
        if (ctx->windows[i].id == window_id)
        {
            *out_window = ctx->windows[i];
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}

int ws_set_cursor_visible(ws_context_t* ctx, bool visible)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    ctx->cursor_visible = visible;
    return 0;
}

int ws_set_cursor_position(ws_context_t* ctx, int32_t x, int32_t y)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    if (ctx->ready)
    {
        int32_t max_x = (int32_t) ctx->mode.hdisplay - 1;
        int32_t max_y = (int32_t) ctx->mode.vdisplay - 1;
        x = ws_clamp_i32(x, 0, max_x);
        y = ws_clamp_i32(y, 0, max_y);
    }

    ctx->cursor_x = x;
    ctx->cursor_y = y;
    return 0;
}

int ws_set_cursor_color(ws_context_t* ctx, uint32_t color)
{
    if (!ctx)
    {
        errno = EINVAL;
        return -1;
    }

    ctx->cursor_color = color;
    return 0;
}

int ws_render(ws_context_t* ctx)
{
    if (!ctx || !ctx->ready || !ctx->frame_map)
    {
        errno = EINVAL;
        return -1;
    }
    if (ctx->dumb_pitch == 0U || ctx->dumb_size == 0U)
    {
        errno = EIO;
        return -1;
    }

    ctx->clip_enabled = false;
    ws_redraw_scene(ctx);

    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_ATOMIC, &ctx->atomic_req) < 0)
        return -1;

    return 0;
}

int ws_render_region(ws_context_t* ctx, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    if (!ctx || !ctx->ready || !ctx->frame_map)
    {
        errno = EINVAL;
        return -1;
    }
    if (ctx->dumb_pitch == 0U || ctx->dumb_size == 0U)
    {
        errno = EIO;
        return -1;
    }

    int32_t clip_x = 0;
    int32_t clip_y = 0;
    uint32_t clip_w = 0U;
    uint32_t clip_h = 0U;
    if (!ws_clip_render_region(ctx, x, y, width, height, &clip_x, &clip_y, &clip_w, &clip_h))
    {
        errno = EINVAL;
        return -1;
    }

    ctx->clip_enabled = true;
    ctx->clip_x0 = clip_x;
    ctx->clip_y0 = clip_y;
    ctx->clip_x1 = clip_x + (int32_t) clip_w;
    ctx->clip_y1 = clip_y + (int32_t) clip_h;
    ws_redraw_scene(ctx);
    ctx->clip_enabled = false;

    drm_mode_atomic_req_t req = ctx->atomic_req;
    bool full_screen = (clip_x == 0 &&
                        clip_y == 0 &&
                        clip_w == (uint32_t) ctx->mode.hdisplay &&
                        clip_h == (uint32_t) ctx->mode.vdisplay);
    if (!full_screen)
    {
        req.src_x = ((uint32_t) clip_x) << 16;
        req.src_y = ((uint32_t) clip_y) << 16;
        req.src_w = clip_w << 16;
        req.src_h = clip_h << 16;
        req.crtc_x = clip_x;
        req.crtc_y = clip_y;
        req.crtc_w = (int32_t) clip_w;
        req.crtc_h = (int32_t) clip_h;
    }

    if (ioctl(ctx->drm_fd, DRM_IOCTL_MODE_ATOMIC, &req) < 0)
        return -1;

    return 0;
}
