#include <Device/DRM.h>

#include <Device/TTY.h>
#include <Device/TTYFramebuffer.h>
#include <Memory/KMem.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static drm_runtime_state_t DRM_state = {
    .next_file_id = 1U,
    .next_buffer_id = 1U,
    .next_blob_id = 1U
};

static uint32_t DRM_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint32_t DRM_align_up_u32(uint32_t value, uint32_t align)
{
    if (align == 0U)
        return value;

    uint32_t mask = align - 1U;
    return (value + mask) & ~mask;
}

static uint32_t DRM_scale_channel(uint8_t value, uint8_t bits)
{
    if (bits == 0)
        return 0;
    if (bits > 31)
        bits = 31;

    uint64_t max_value = (1ULL << bits) - 1ULL;
    return (uint32_t) ((((uint64_t) value * max_value) + 127ULL) / 255ULL);
}

static uint32_t DRM_encode_rgb(const TTYFB_runtime_state_t* fb, uint8_t r, uint8_t g, uint8_t b)
{
    if (!fb)
        return 0;

    uint32_t pixel = 0;
    if (fb->red_pos < 32)
        pixel |= DRM_scale_channel(r, fb->red_size) << fb->red_pos;
    if (fb->green_pos < 32)
        pixel |= DRM_scale_channel(g, fb->green_size) << fb->green_pos;
    if (fb->blue_pos < 32)
        pixel |= DRM_scale_channel(b, fb->blue_size) << fb->blue_pos;
    return pixel;
}

static void DRM_store_fb_pixel(const TTYFB_runtime_state_t* fb, uint8_t* target, uint32_t x, uint32_t y, uint32_t pixel)
{
    if (!fb || !target)
        return;
    if (x >= fb->width || y >= fb->height)
        return;

    size_t offset = (size_t) y * fb->pitch + ((size_t) x * fb->bytes_per_pixel);
    uint8_t* dst = target + offset;
    switch (fb->bytes_per_pixel)
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
        default:
            break;
    }
}

static bool DRM_get_runtime_mode(drm_mode_modeinfo_t* out_mode, TTYFB_runtime_state_t* out_fb)
{
    TTYFB_runtime_state_t fb;
    if (!TTY_has_framebuffer())
        return false;
    if (!TTYFB_get_runtime_state(&fb) || !fb.ready || fb.width == 0 || fb.height == 0)
        return false;

    if (out_fb)
        *out_fb = fb;

    if (out_mode)
    {
        memset(out_mode, 0, sizeof(*out_mode));
        out_mode->hdisplay = (uint16_t) DRM_min_u32(fb.width, 0xFFFFU);
        out_mode->hsync_start = out_mode->hdisplay;
        out_mode->hsync_end = out_mode->hdisplay;
        out_mode->htotal = out_mode->hdisplay;
        out_mode->vdisplay = (uint16_t) DRM_min_u32(fb.height, 0xFFFFU);
        out_mode->vsync_start = out_mode->vdisplay;
        out_mode->vsync_end = out_mode->vdisplay;
        out_mode->vtotal = out_mode->vdisplay;
        out_mode->vrefresh = 60U;
        memcpy(out_mode->name, "TTYFB", 6U);
    }

    return true;
}

static bool DRM_refresh_mode_locked(void)
{
    drm_mode_modeinfo_t mode;
    if (!DRM_get_runtime_mode(&mode, NULL))
    {
        DRM_state.ready = false;
        return false;
    }

    DRM_state.ready = true;
    DRM_state.preferred_mode = mode;
    if (!DRM_state.crtc.mode_valid)
    {
        DRM_state.crtc.mode = mode;
        DRM_state.crtc.mode_valid = true;
        DRM_state.crtc.active = false;
        DRM_state.crtc.fb_handle = 0;
        DRM_state.crtc.fb_buffer_id = 0;
        DRM_state.crtc.owner_file_id = 0;

        DRM_state.plane.fb_handle = 0;
        DRM_state.plane.fb_buffer_id = 0;
        DRM_state.plane.crtc_id = DRM_CRTC_ID;
        DRM_state.plane.src_x = 0;
        DRM_state.plane.src_y = 0;
        DRM_state.plane.src_w = (uint32_t) mode.hdisplay << 16;
        DRM_state.plane.src_h = (uint32_t) mode.vdisplay << 16;
        DRM_state.plane.crtc_x = 0;
        DRM_state.plane.crtc_y = 0;
        DRM_state.plane.crtc_w = mode.hdisplay;
        DRM_state.plane.crtc_h = mode.vdisplay;

        DRM_state.connector.crtc_id = 0;
    }

    return true;
}

static drm_file_t* DRM_file_find_locked(uint32_t file_id)
{
    if (file_id == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_FILES; i++)
    {
        drm_file_t* file = &DRM_state.files[i];
        if (file->used && file->file_id == file_id)
            return file;
    }

    return NULL;
}

static drm_buffer_t* DRM_buffer_find_by_id_locked(uint32_t buffer_id)
{
    if (buffer_id == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_BUFFERS; i++)
    {
        drm_buffer_t* buffer = &DRM_state.buffers[i];
        if (buffer->used && buffer->buffer_id == buffer_id)
            return buffer;
    }

    return NULL;
}

static drm_buffer_t* DRM_buffer_find_by_phys_locked(uintptr_t phys)
{
    if (phys == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_BUFFERS; i++)
    {
        drm_buffer_t* buffer = &DRM_state.buffers[i];
        if (!buffer->used || !buffer->page_phys)
            continue;

        for (uint32_t page = 0; page < buffer->page_count; page++)
        {
            if (buffer->page_phys[page] == phys)
                return buffer;
        }
    }

    return NULL;
}

static drm_mode_blob_t* DRM_blob_find_by_id_locked(uint32_t blob_id)
{
    if (blob_id == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_MODE_BLOBS; i++)
    {
        drm_mode_blob_t* blob = &DRM_state.blobs[i];
        if (blob->used && blob->blob_id == blob_id)
            return blob;
    }

    return NULL;
}

static drm_file_handle_t* DRM_file_find_handle_locked(drm_file_t* file, uint32_t handle)
{
    if (!file || handle == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_FILE_HANDLES; i++)
    {
        drm_file_handle_t* file_handle = &file->handles[i];
        if (file_handle->used && file_handle->handle == handle)
            return file_handle;
    }

    return NULL;
}

static drm_file_handle_t* DRM_file_find_handle_for_buffer_locked(drm_file_t* file, uint32_t buffer_id)
{
    if (!file || buffer_id == 0)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_FILE_HANDLES; i++)
    {
        drm_file_handle_t* file_handle = &file->handles[i];
        if (file_handle->used && file_handle->buffer_id == buffer_id)
            return file_handle;
    }

    return NULL;
}

static drm_file_handle_t* DRM_file_alloc_handle_locked(drm_file_t* file)
{
    if (!file)
        return NULL;

    for (uint32_t i = 0; i < DRM_MAX_FILE_HANDLES; i++)
    {
        drm_file_handle_t* file_handle = &file->handles[i];
        if (file_handle->used)
            continue;

        memset(file_handle, 0, sizeof(*file_handle));
        file_handle->used = true;
        file_handle->handle = i + 1U;
        return file_handle;
    }

    return NULL;
}

static drm_buffer_t* DRM_alloc_buffer_slot_locked(void)
{
    for (uint32_t i = 0; i < DRM_MAX_BUFFERS; i++)
    {
        if (!DRM_state.buffers[i].used)
            return &DRM_state.buffers[i];
    }

    return NULL;
}

static drm_mode_blob_t* DRM_alloc_blob_slot_locked(void)
{
    for (uint32_t i = 0; i < DRM_MAX_MODE_BLOBS; i++)
    {
        if (!DRM_state.blobs[i].used)
            return &DRM_state.blobs[i];
    }

    return NULL;
}

static void DRM_release_buffer_if_unused_locked(drm_buffer_t* buffer)
{
    if (!buffer || !buffer->used)
        return;

    if (buffer->handle_refs != 0 || buffer->fd_refs != 0 || buffer->map_page_refs != 0)
        return;

    if (buffer->page_phys)
    {
        for (uint32_t i = 0; i < buffer->page_count; i++)
        {
            if (buffer->page_phys[i] != 0)
                PMM_dealloc_page((void*) buffer->page_phys[i]);
        }

        kfree(buffer->page_phys);
    }

    memset(buffer, 0, sizeof(*buffer));
}

static bool DRM_buffer_read_xrgb8888(const drm_buffer_t* buffer, uint32_t x, uint32_t y, uint32_t* out_pixel)
{
    if (!buffer || !buffer->used || !buffer->page_phys || !out_pixel)
        return false;
    if (x >= buffer->width || y >= buffer->height)
        return false;

    uint64_t offset = ((uint64_t) y * (uint64_t) buffer->pitch) + ((uint64_t) x * 4ULL);
    if (offset + 4ULL > buffer->size)
        return false;

    uint8_t pixel_bytes[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 4U; i++)
    {
        uint64_t byte_offset = offset + i;
        uint32_t page_index = (uint32_t) (byte_offset / PHYS_PAGE_SIZE);
        uint32_t page_off = (uint32_t) (byte_offset % PHYS_PAGE_SIZE);
        if (page_index >= buffer->page_count)
            return false;

        uint8_t* page = (uint8_t*) P2V(buffer->page_phys[page_index]);
        pixel_bytes[i] = page[page_off];
    }

    *out_pixel = ((uint32_t) pixel_bytes[0]) |
                 ((uint32_t) pixel_bytes[1] << 8) |
                 ((uint32_t) pixel_bytes[2] << 16) |
                 ((uint32_t) pixel_bytes[3] << 24);
    return true;
}

static bool DRM_present_buffer_locked(const drm_buffer_t* buffer,
                                      const drm_mode_atomic_req_t* req,
                                      const drm_mode_modeinfo_t* mode)
{
    if (!buffer || !req || !mode)
        return false;

    TTYFB_runtime_state_t fb;
    if (!TTYFB_get_runtime_state(&fb) || !fb.ready || !fb.front)
        return false;

    if (fb.width == 0 || fb.height == 0 || fb.pitch == 0 || fb.bytes_per_pixel == 0)
        return false;

    if (req->crtc_x < 0 || req->crtc_y < 0)
        return false;

    uint32_t src_x = req->src_x >> 16;
    uint32_t src_y = req->src_y >> 16;
    uint32_t src_w = req->src_w >> 16;
    uint32_t src_h = req->src_h >> 16;
    uint32_t dst_x = (uint32_t) req->crtc_x;
    uint32_t dst_y = (uint32_t) req->crtc_y;
    uint32_t dst_w = req->crtc_w;
    uint32_t dst_h = req->crtc_h;

    if (src_w == 0)
        src_w = buffer->width;
    if (src_h == 0)
        src_h = buffer->height;
    if (dst_w == 0)
        dst_w = (uint32_t) mode->hdisplay;
    if (dst_h == 0)
        dst_h = (uint32_t) mode->vdisplay;

    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return false;

    if (src_x >= buffer->width || src_y >= buffer->height)
        return false;
    if ((uint64_t) src_x + src_w > buffer->width)
        return false;
    if ((uint64_t) src_y + src_h > buffer->height)
        return false;

    if (dst_x >= fb.width || dst_y >= fb.height)
        return false;
    if ((uint64_t) dst_x + dst_w > fb.width)
        return false;
    if ((uint64_t) dst_y + dst_h > fb.height)
        return false;

    for (uint32_t dy = 0; dy < dst_h; dy++)
    {
        uint32_t sy = src_y + (uint32_t) (((uint64_t) dy * src_h) / dst_h);
        for (uint32_t dx = 0; dx < dst_w; dx++)
        {
            uint32_t sx = src_x + (uint32_t) (((uint64_t) dx * src_w) / dst_w);

            uint32_t src_pixel = 0;
            if (!DRM_buffer_read_xrgb8888(buffer, sx, sy, &src_pixel))
                return false;

            uint8_t b = (uint8_t) (src_pixel & 0xFFU);
            uint8_t g = (uint8_t) ((src_pixel >> 8) & 0xFFU);
            uint8_t r = (uint8_t) ((src_pixel >> 16) & 0xFFU);
            uint32_t out_pixel = DRM_encode_rgb(&fb, r, g, b);

            DRM_store_fb_pixel(&fb, fb.front, dst_x + dx, dst_y + dy, out_pixel);
            if (fb.double_buffer && fb.back)
                DRM_store_fb_pixel(&fb, fb.back, dst_x + dx, dst_y + dy, out_pixel);
        }
    }

    return true;
}

void DRM_init(void)
{
    if (!DRM_state.lock_ready)
    {
        spinlock_init(&DRM_state.lock);
        DRM_state.lock_ready = true;
    }

    spin_lock(&DRM_state.lock);
    (void) DRM_refresh_mode_locked();
    spin_unlock(&DRM_state.lock);
}

bool DRM_is_available(void)
{
    drm_mode_modeinfo_t mode;
    return DRM_get_runtime_mode(&mode, NULL);
}

bool DRM_open_file(uint32_t owner_pid, uint32_t* out_file_id)
{
    if (!out_file_id || owner_pid == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_refresh_mode_locked())
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_file_t* file = NULL;
    for (uint32_t i = 0; i < DRM_MAX_FILES; i++)
    {
        if (!DRM_state.files[i].used)
        {
            file = &DRM_state.files[i];
            break;
        }
    }

    if (!file)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    memset(file, 0, sizeof(*file));
    file->used = true;
    file->owner_pid = owner_pid;
    file->file_id = DRM_state.next_file_id++;
    if (file->file_id == 0)
        file->file_id = DRM_state.next_file_id++;

    *out_file_id = file->file_id;
    spin_unlock(&DRM_state.lock);
    return true;
}

void DRM_close_file(uint32_t file_id)
{
    if (file_id == 0 || !DRM_state.lock_ready)
        return;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    if (!file)
    {
        spin_unlock(&DRM_state.lock);
        return;
    }

    bool disable_scanout = false;
    for (uint32_t i = 0; i < DRM_MAX_FILE_HANDLES; i++)
    {
        drm_file_handle_t* file_handle = &file->handles[i];
        if (!file_handle->used)
            continue;

        if (DRM_state.crtc.owner_file_id == file_id &&
            DRM_state.plane.fb_buffer_id == file_handle->buffer_id)
            disable_scanout = true;

        drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(file_handle->buffer_id);
        if (buffer && buffer->handle_refs > 0)
            buffer->handle_refs--;

        memset(file_handle, 0, sizeof(*file_handle));
    }

    if (disable_scanout)
    {
        DRM_state.connector.crtc_id = 0;
        DRM_state.crtc.active = false;
        DRM_state.crtc.fb_handle = 0;
        DRM_state.crtc.fb_buffer_id = 0;
        DRM_state.crtc.owner_file_id = 0;
        DRM_state.plane.fb_handle = 0;
        DRM_state.plane.fb_buffer_id = 0;
    }

    memset(file, 0, sizeof(*file));

    for (uint32_t i = 0; i < DRM_MAX_BUFFERS; i++)
        DRM_release_buffer_if_unused_locked(&DRM_state.buffers[i]);

    if (!DRM_state.crtc.active && DRM_state.tty_disabled)
    {
        TTY_set_output_enabled(true);
        DRM_state.tty_disabled = false;
    }

    spin_unlock(&DRM_state.lock);
}

bool DRM_get_resources(uint32_t file_id, drm_mode_get_resources_t* io)
{
    if (!io || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    if (!file)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (!DRM_refresh_mode_locked())
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    io->count_crtcs = 1;
    io->count_connectors = 1;
    io->count_planes = 1;
    io->min_width = 16U;
    io->min_height = 16U;
    io->max_width = DRM_state.preferred_mode.hdisplay;
    io->max_height = DRM_state.preferred_mode.vdisplay;
    (void) file;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_get_connector(uint32_t file_id,
                       drm_mode_get_connector_t* io,
                       drm_mode_modeinfo_t* out_modes,
                       uint32_t modes_capacity,
                       uint32_t* out_modes_written)
{
    if (!io || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_file_find_locked(file_id))
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (!DRM_refresh_mode_locked())
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (io->connector_id != 0 && io->connector_id != DRM_CONNECTOR_ID)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    io->connector_id = DRM_CONNECTOR_ID;
    io->connection = DRM_MODE_CONNECTED;
    io->count_modes = 1U;
    io->mm_width = (DRM_state.preferred_mode.hdisplay * 254U) / 960U;
    io->mm_height = (DRM_state.preferred_mode.vdisplay * 254U) / 960U;

    if (out_modes_written)
        *out_modes_written = 0;
    if (out_modes && modes_capacity > 0)
    {
        out_modes[0] = DRM_state.preferred_mode;
        if (out_modes_written)
            *out_modes_written = 1;
    }

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_get_crtc(uint32_t file_id, drm_mode_get_crtc_t* io)
{
    if (!io || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_file_find_locked(file_id))
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (!DRM_refresh_mode_locked())
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (io->crtc_id != 0 && io->crtc_id != DRM_CRTC_ID)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    io->crtc_id = DRM_CRTC_ID;
    io->fb_handle = DRM_state.crtc.fb_handle;
    io->x = DRM_state.plane.crtc_x;
    io->y = DRM_state.plane.crtc_y;
    io->active = DRM_state.crtc.active ? 1U : 0U;
    io->mode_valid = DRM_state.crtc.mode_valid ? 1U : 0U;
    io->mode = DRM_state.crtc.mode_valid ? DRM_state.crtc.mode : DRM_state.preferred_mode;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_get_plane(uint32_t file_id,
                   drm_mode_get_plane_t* io,
                   uint32_t* out_formats,
                   uint32_t formats_capacity,
                   uint32_t* out_formats_written)
{
    if (!io || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_file_find_locked(file_id))
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (io->plane_id != 0 && io->plane_id != DRM_PLANE_ID)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    io->plane_id = DRM_PLANE_ID;
    io->crtc_id = DRM_state.plane.crtc_id;
    io->fb_handle = DRM_state.plane.fb_handle;
    io->possible_crtcs = 1U;
    io->count_format_types = 1U;
    io->plane_type = DRM_PLANE_TYPE_PRIMARY;

    if (out_formats_written)
        *out_formats_written = 0;
    if (out_formats && formats_capacity > 0)
    {
        out_formats[0] = DRM_FORMAT_XRGB8888;
        if (out_formats_written)
            *out_formats_written = 1;
    }

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_create_dumb(uint32_t file_id, drm_mode_create_dumb_t* io)
{
    if (!io || !DRM_state.lock_ready)
        return false;

    if (io->width == 0 || io->height == 0 || io->bpp != 32U)
        return false;

    uint64_t pitch64 = (uint64_t) io->width * 4ULL;
    if (pitch64 > 0xFFFFFFFFULL)
        return false;

    uint32_t pitch = DRM_align_up_u32((uint32_t) pitch64, 64U);
    uint64_t size64 = (uint64_t) pitch * (uint64_t) io->height;
    if (size64 == 0 || size64 > (uint64_t) DRM_MAX_PAGES_PER_BUFFER * (uint64_t) PHYS_PAGE_SIZE)
        return false;

    uint32_t page_count = (uint32_t) ((size64 + PHYS_PAGE_SIZE - 1ULL) / PHYS_PAGE_SIZE);
    if (page_count == 0 || page_count > DRM_MAX_PAGES_PER_BUFFER)
        return false;

    uintptr_t* page_phys = (uintptr_t*) kmalloc((size_t) page_count * sizeof(uintptr_t));
    if (!page_phys)
        return false;

    for (uint32_t i = 0; i < page_count; i++)
        page_phys[i] = 0;

    bool alloc_ok = true;
    for (uint32_t i = 0; i < page_count; i++)
    {
        uintptr_t phys = (uintptr_t) PMM_alloc_page();
        if (phys == 0)
        {
            alloc_ok = false;
            break;
        }

        page_phys[i] = phys;
        memset((void*) P2V(phys), 0, PHYS_PAGE_SIZE);
    }

    if (!alloc_ok)
    {
        for (uint32_t i = 0; i < page_count; i++)
        {
            if (page_phys[i] != 0)
                PMM_dealloc_page((void*) page_phys[i]);
        }
        kfree(page_phys);
        return false;
    }

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    drm_buffer_t* buffer = DRM_alloc_buffer_slot_locked();
    drm_file_handle_t* file_handle = DRM_file_alloc_handle_locked(file);
    if (!file || !buffer || !file_handle)
    {
        spin_unlock(&DRM_state.lock);
        for (uint32_t i = 0; i < page_count; i++)
            PMM_dealloc_page((void*) page_phys[i]);
        kfree(page_phys);
        return false;
    }

    memset(buffer, 0, sizeof(*buffer));
    buffer->used = true;
    buffer->buffer_id = DRM_state.next_buffer_id++;
    if (buffer->buffer_id == 0)
        buffer->buffer_id = DRM_state.next_buffer_id++;
    buffer->width = io->width;
    buffer->height = io->height;
    buffer->bpp = io->bpp;
    buffer->format = DRM_FORMAT_XRGB8888;
    buffer->pitch = pitch;
    buffer->size = size64;
    buffer->page_count = page_count;
    buffer->page_phys = page_phys;
    buffer->handle_refs = 1;
    buffer->fd_refs = 0;
    buffer->map_page_refs = 0;

    file_handle->buffer_id = buffer->buffer_id;

    io->handle = file_handle->handle;
    io->pitch = pitch;
    io->size = size64;

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_destroy_dumb(uint32_t file_id, uint32_t handle)
{
    if (handle == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    drm_file_handle_t* file_handle = DRM_file_find_handle_locked(file, handle);
    if (!file || !file_handle)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(file_handle->buffer_id);
    if (!buffer)
    {
        memset(file_handle, 0, sizeof(*file_handle));
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (DRM_state.crtc.owner_file_id == file_id &&
        DRM_state.crtc.fb_buffer_id == file_handle->buffer_id)
    {
        DRM_state.crtc.fb_handle = 0;
        DRM_state.crtc.fb_buffer_id = 0;
        DRM_state.crtc.owner_file_id = 0;
        DRM_state.crtc.active = false;
        DRM_state.plane.fb_handle = 0;
        DRM_state.plane.fb_buffer_id = 0;
        DRM_state.connector.crtc_id = 0;
    }

    if (buffer->handle_refs > 0)
        buffer->handle_refs--;
    memset(file_handle, 0, sizeof(*file_handle));
    DRM_release_buffer_if_unused_locked(buffer);

    if (!DRM_state.crtc.active && DRM_state.tty_disabled)
    {
        TTY_set_output_enabled(true);
        DRM_state.tty_disabled = false;
    }

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_create_mode_blob(uint32_t file_id, const void* data, uint32_t length, uint32_t* out_blob_id)
{
    if (!data || !out_blob_id || !DRM_state.lock_ready)
        return false;
    if (length != sizeof(drm_mode_modeinfo_t))
        return false;

    drm_mode_modeinfo_t mode;
    memcpy(&mode, data, sizeof(mode));
    if (mode.hdisplay == 0 || mode.vdisplay == 0)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_file_find_locked(file_id))
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_mode_blob_t* blob = DRM_alloc_blob_slot_locked();
    if (!blob)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    memset(blob, 0, sizeof(*blob));
    blob->used = true;
    blob->blob_id = DRM_state.next_blob_id++;
    if (blob->blob_id == 0)
        blob->blob_id = DRM_state.next_blob_id++;
    blob->length = sizeof(drm_mode_modeinfo_t);
    blob->mode = mode;

    *out_blob_id = blob->blob_id;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_destroy_mode_blob(uint32_t file_id, uint32_t blob_id)
{
    if (blob_id == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    if (!DRM_file_find_locked(file_id))
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_mode_blob_t* blob = DRM_blob_find_by_id_locked(blob_id);
    if (!blob)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    memset(blob, 0, sizeof(*blob));
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_prime_handle_to_dmabuf(uint32_t file_id, uint32_t handle, uint32_t* out_dmabuf_id)
{
    if (!out_dmabuf_id || handle == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    drm_file_handle_t* file_handle = DRM_file_find_handle_locked(file, handle);
    if (!file || !file_handle)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(file_handle->buffer_id);
    if (!buffer)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    *out_dmabuf_id = buffer->buffer_id;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_prime_dmabuf_to_handle(uint32_t file_id, uint32_t dmabuf_id, uint32_t* out_handle)
{
    if (!out_handle || dmabuf_id == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (!file || !buffer)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_file_handle_t* existing = DRM_file_find_handle_for_buffer_locked(file, dmabuf_id);
    if (existing)
    {
        *out_handle = existing->handle;
        spin_unlock(&DRM_state.lock);
        return true;
    }

    drm_file_handle_t* file_handle = DRM_file_alloc_handle_locked(file);
    if (!file_handle)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    file_handle->buffer_id = dmabuf_id;
    if (buffer->handle_refs == UINT32_MAX)
    {
        memset(file_handle, 0, sizeof(*file_handle));
        spin_unlock(&DRM_state.lock);
        return false;
    }

    buffer->handle_refs++;
    *out_handle = file_handle->handle;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_atomic_commit(uint32_t file_id, const drm_mode_atomic_req_t* req)
{
    if (!req || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_file_t* file = DRM_file_find_locked(file_id);
    if (!file)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (req->connector_id != DRM_CONNECTOR_ID || req->crtc_id != DRM_CRTC_ID || req->plane_id != DRM_PLANE_ID)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    drm_mode_modeinfo_t mode = DRM_state.crtc.mode_valid ? DRM_state.crtc.mode : DRM_state.preferred_mode;
    if (req->mode_blob_id != 0)
    {
        drm_mode_blob_t* blob = DRM_blob_find_by_id_locked(req->mode_blob_id);
        if (!blob)
        {
            spin_unlock(&DRM_state.lock);
            return false;
        }
        mode = blob->mode;
    }

    drm_buffer_t* buffer = NULL;
    if (req->active != 0)
    {
        drm_file_handle_t* file_handle = DRM_file_find_handle_locked(file, req->fb_handle);
        if (!file_handle)
        {
            spin_unlock(&DRM_state.lock);
            return false;
        }

        buffer = DRM_buffer_find_by_id_locked(file_handle->buffer_id);
        if (!buffer)
        {
            spin_unlock(&DRM_state.lock);
            return false;
        }
    }

    if ((req->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0)
    {
        spin_unlock(&DRM_state.lock);
        return true;
    }

    if (req->active != 0)
    {
        if (!DRM_refresh_mode_locked())
        {
            spin_unlock(&DRM_state.lock);
            return false;
        }

        if (!DRM_present_buffer_locked(buffer, req, &mode))
        {
            spin_unlock(&DRM_state.lock);
            return false;
        }

        DRM_state.connector.crtc_id = DRM_CRTC_ID;
        DRM_state.crtc.active = true;
        DRM_state.crtc.fb_handle = req->fb_handle;
        DRM_state.crtc.fb_buffer_id = buffer->buffer_id;
        DRM_state.crtc.owner_file_id = file_id;
        DRM_state.crtc.mode = mode;
        DRM_state.crtc.mode_valid = true;
        DRM_state.plane.fb_handle = req->fb_handle;
        DRM_state.plane.fb_buffer_id = buffer->buffer_id;
        DRM_state.plane.crtc_id = DRM_CRTC_ID;
        DRM_state.plane.src_x = req->src_x;
        DRM_state.plane.src_y = req->src_y;
        DRM_state.plane.src_w = req->src_w;
        DRM_state.plane.src_h = req->src_h;
        DRM_state.plane.crtc_x = req->crtc_x;
        DRM_state.plane.crtc_y = req->crtc_y;
        DRM_state.plane.crtc_w = req->crtc_w;
        DRM_state.plane.crtc_h = req->crtc_h;

        if (!DRM_state.tty_disabled)
        {
            TTY_set_output_enabled(false);
            DRM_state.tty_disabled = true;
        }
    }
    else
    {
        DRM_state.connector.crtc_id = 0;
        DRM_state.crtc.active = false;
        DRM_state.crtc.fb_handle = 0;
        DRM_state.crtc.fb_buffer_id = 0;
        DRM_state.crtc.owner_file_id = 0;
        DRM_state.plane.fb_handle = 0;
        DRM_state.plane.fb_buffer_id = 0;

        if (DRM_state.tty_disabled)
        {
            TTY_set_output_enabled(true);
            DRM_state.tty_disabled = false;
        }
    }

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_ref_fd(uint32_t dmabuf_id)
{
    if (dmabuf_id == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (!buffer || buffer->fd_refs == UINT32_MAX)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    buffer->fd_refs++;
    spin_unlock(&DRM_state.lock);
    return true;
}

void DRM_dmabuf_unref_fd(uint32_t dmabuf_id)
{
    if (dmabuf_id == 0 || !DRM_state.lock_ready)
        return;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (buffer && buffer->fd_refs > 0)
    {
        buffer->fd_refs--;
        DRM_release_buffer_if_unused_locked(buffer);
    }
    spin_unlock(&DRM_state.lock);
}

bool DRM_dmabuf_get_layout(uint32_t dmabuf_id, uint64_t* out_size, uint32_t* out_page_count)
{
    if (dmabuf_id == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (!buffer)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (out_size)
        *out_size = buffer->size;
    if (out_page_count)
        *out_page_count = buffer->page_count;

    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_get_page_phys(uint32_t dmabuf_id, uint32_t page_index, uintptr_t* out_phys)
{
    if (dmabuf_id == 0 || !out_phys || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (!buffer || !buffer->page_phys || page_index >= buffer->page_count)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    *out_phys = buffer->page_phys[page_index];
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_ref_map_pages(uint32_t dmabuf_id, uint32_t page_count)
{
    if (dmabuf_id == 0 || !DRM_state.lock_ready)
        return false;
    if (page_count == 0)
        return true;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_id_locked(dmabuf_id);
    if (!buffer || buffer->map_page_refs > UINT32_MAX - page_count)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    buffer->map_page_refs += page_count;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_ref_map_pages_by_phys(uintptr_t phys, uint32_t page_count)
{
    if (phys == 0 || !DRM_state.lock_ready)
        return false;
    if (page_count == 0)
        return true;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_phys_locked(phys);
    if (!buffer || buffer->map_page_refs > UINT32_MAX - page_count)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    buffer->map_page_refs += page_count;
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_unref_map_pages_by_phys(uintptr_t phys, uint32_t page_count)
{
    if (phys == 0 || !DRM_state.lock_ready)
        return false;
    if (page_count == 0)
        return true;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_phys_locked(phys);
    if (!buffer || buffer->map_page_refs < page_count)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    buffer->map_page_refs -= page_count;
    DRM_release_buffer_if_unused_locked(buffer);
    spin_unlock(&DRM_state.lock);
    return true;
}

bool DRM_dmabuf_contains_phys(uintptr_t phys, uint32_t* out_dmabuf_id)
{
    if (phys == 0 || !DRM_state.lock_ready)
        return false;

    spin_lock(&DRM_state.lock);
    drm_buffer_t* buffer = DRM_buffer_find_by_phys_locked(phys);
    if (!buffer)
    {
        spin_unlock(&DRM_state.lock);
        return false;
    }

    if (out_dmabuf_id)
        *out_dmabuf_id = buffer->buffer_id;
    spin_unlock(&DRM_state.lock);
    return true;
}
