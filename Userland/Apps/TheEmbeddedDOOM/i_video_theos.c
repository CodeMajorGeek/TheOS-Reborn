#include <doomdef.h>
#include <doomstat.h>
#include <d_main.h>
#include <i_system.h>
#include <m_argv.h>
#include <v_video.h>

#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syscall.h>
#include <unistd.h>

typedef struct doom_drm_state
{
    bool ready;
    bool e0_prefix;
    int card_fd;
    int dmabuf_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t dumb_handle;
    uint32_t dumb_pitch;
    uint64_t dumb_size;
    uint32_t mode_blob_id;
    drm_mode_modeinfo_t mode;
    drm_mode_atomic_req_t atomic_req;
    uint8_t* frame_map;
    uint8_t palette[256 * 3];
} doom_drm_state_t;

static doom_drm_state_t DoomDRM = {
    .card_fd = -1,
    .dmabuf_fd = -1
};

static void doom_post_key_event(int key, bool down)
{
    if (key == 0)
        return;

    event_t event;
    event.type = down ? ev_keydown : ev_keyup;
    event.data1 = key;
    event.data2 = 0;
    event.data3 = 0;
    D_PostEvent(&event);
}

static int doom_map_scancode_to_key(uint8_t scancode, bool extended)
{
    if (extended)
    {
        switch (scancode)
        {
            case 0x48:
                return KEY_UPARROW;
            case 0x50:
                return KEY_DOWNARROW;
            case 0x4B:
                return KEY_LEFTARROW;
            case 0x4D:
                return KEY_RIGHTARROW;
            case 0x53:
                return KEY_BACKSPACE;
            case 0x1D:
                return KEY_RCTRL;
            case 0x38:
                return KEY_RALT;
            case 0x1C:
                return KEY_ENTER;
            default:
                return 0;
        }
    }

    switch (scancode)
    {
        case 0x01:
            return KEY_ESCAPE;
        case 0x0E:
            return KEY_BACKSPACE;
        case 0x0F:
            return KEY_TAB;
        case 0x1C:
            return KEY_ENTER;
        case 0x1D:
            return KEY_RCTRL;
        case 0x2A:
        case 0x36:
            return KEY_RSHIFT;
        case 0x38:
            return KEY_RALT;
        case 0x3B:
            return KEY_F1;
        case 0x3C:
            return KEY_F2;
        case 0x3D:
            return KEY_F3;
        case 0x3E:
            return KEY_F4;
        case 0x3F:
            return KEY_F5;
        case 0x40:
            return KEY_F6;
        case 0x41:
            return KEY_F7;
        case 0x42:
            return KEY_F8;
        case 0x43:
            return KEY_F9;
        case 0x44:
            return KEY_F10;
        case 0x57:
            return KEY_F11;
        case 0x58:
            return KEY_F12;
        case 0x45:
            return KEY_PAUSE;
        case 0x0C:
            return KEY_MINUS;
        case 0x0D:
            return KEY_EQUALS;
        case 0x39:
            return ' ';

        case 0x02:
            return '1';
        case 0x03:
            return '2';
        case 0x04:
            return '3';
        case 0x05:
            return '4';
        case 0x06:
            return '5';
        case 0x07:
            return '6';
        case 0x08:
            return '7';
        case 0x09:
            return '8';
        case 0x0A:
            return '9';
        case 0x0B:
            return '0';
        case 0x10:
            return 'q';
        case 0x11:
            return 'w';
        case 0x12:
            return 'e';
        case 0x13:
            return 'r';
        case 0x14:
            return 't';
        case 0x15:
            return 'y';
        case 0x16:
            return 'u';
        case 0x17:
            return 'i';
        case 0x18:
            return 'o';
        case 0x19:
            return 'p';
        case 0x1A:
            return '[';
        case 0x1B:
            return ']';
        case 0x1E:
            return 'a';
        case 0x1F:
            return 's';
        case 0x20:
            return 'd';
        case 0x21:
            return 'f';
        case 0x22:
            return 'g';
        case 0x23:
            return 'h';
        case 0x24:
            return 'j';
        case 0x25:
            return 'k';
        case 0x26:
            return 'l';
        case 0x27:
            return ';';
        case 0x28:
            return '\'';
        case 0x29:
            return '`';
        case 0x2B:
            return '\\';
        case 0x2C:
            return 'z';
        case 0x2D:
            return 'x';
        case 0x2E:
            return 'c';
        case 0x2F:
            return 'v';
        case 0x30:
            return 'b';
        case 0x31:
            return 'n';
        case 0x32:
            return 'm';
        case 0x33:
            return ',';
        case 0x34:
            return '.';
        case 0x35:
            return '/';
        default:
            return 0;
    }
}

static void doom_poll_keyboard(void)
{
    for (;;)
    {
        int raw = sys_kbd_get_scancode();
        if (raw <= 0)
            return;

        uint8_t scancode = (uint8_t) raw;
        if (scancode == 0xE0U)
        {
            DoomDRM.e0_prefix = true;
            continue;
        }

        bool release = (scancode & 0x80U) != 0;
        uint8_t make_code = (uint8_t) (scancode & 0x7FU);
        int key = doom_map_scancode_to_key(make_code, DoomDRM.e0_prefix);
        DoomDRM.e0_prefix = false;
        doom_post_key_event(key, !release);
    }
}

static void doom_drm_destroy(void)
{
    if (DoomDRM.card_fd >= 0)
    {
        if (DoomDRM.mode_blob_id != 0)
        {
            drm_mode_destroy_blob_t destroy_blob = { .blob_id = DoomDRM.mode_blob_id };
            (void) ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_DESTROY_BLOB, &destroy_blob);
            DoomDRM.mode_blob_id = 0;
        }

        if (DoomDRM.dumb_handle != 0)
        {
            drm_mode_destroy_dumb_t destroy_dumb = { .handle = DoomDRM.dumb_handle };
            (void) ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
            DoomDRM.dumb_handle = 0;
        }
    }

    if (DoomDRM.frame_map && DoomDRM.dumb_size != 0)
    {
        (void) munmap(DoomDRM.frame_map, (size_t) DoomDRM.dumb_size);
        DoomDRM.frame_map = NULL;
    }

    if (DoomDRM.dmabuf_fd >= 0)
    {
        (void) close(DoomDRM.dmabuf_fd);
        DoomDRM.dmabuf_fd = -1;
    }

    if (DoomDRM.card_fd >= 0)
    {
        (void) close(DoomDRM.card_fd);
        DoomDRM.card_fd = -1;
    }

    DoomDRM.ready = false;
}

static bool doom_drm_init(void)
{
    DoomDRM.card_fd = open(DRM_NODE_PATH, O_RDWR);
    if (DoomDRM.card_fd < 0)
    {
        printf("[DOOM] DRM open failed path=%s errno=%d\n", DRM_NODE_PATH, errno);
        return false;
    }

    drm_mode_get_resources_t resources;
    memset(&resources, 0, sizeof(resources));
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    resources.count_connectors = 1;
    resources.count_crtcs = 1;
    resources.count_planes = 1;
    resources.connector_id_ptr = (uint64_t) (uintptr_t) &connector_id;
    resources.crtc_id_ptr = (uint64_t) (uintptr_t) &crtc_id;
    resources.plane_id_ptr = (uint64_t) (uintptr_t) &plane_id;
    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_GET_RESOURCES, &resources) < 0)
    {
        printf("[DOOM] DRM get resources failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    drm_mode_modeinfo_t mode;
    memset(&mode, 0, sizeof(mode));
    drm_mode_get_connector_t connector;
    memset(&connector, 0, sizeof(connector));
    connector.connector_id = connector_id;
    connector.count_modes = 1;
    connector.modes_ptr = (uint64_t) (uintptr_t) &mode;
    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_GET_CONNECTOR, &connector) < 0)
    {
        printf("[DOOM] DRM get connector failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    if (mode.hdisplay == 0 || mode.vdisplay == 0)
    {
        mode.hdisplay = (uint16_t) resources.max_width;
        mode.vdisplay = (uint16_t) resources.max_height;
        mode.vrefresh = 60U;
    }

    drm_mode_create_dumb_t dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = SCREENWIDTH;
    dumb.height = SCREENHEIGHT;
    dumb.bpp = 32U;
    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0)
    {
        printf("[DOOM] DRM create dumb failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    drm_prime_handle_t prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = dumb.handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
    {
        printf("[DOOM] DRM handle->fd failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    void* map = mmap(NULL,
                     (size_t) dumb.size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     prime.fd,
                     0);
    if (map == MAP_FAILED)
    {
        printf("[DOOM] DRM mmap failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    drm_mode_create_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t) (uintptr_t) &mode;
    blob.length = sizeof(mode);
    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_CREATE_BLOB, &blob) < 0)
    {
        printf("[DOOM] DRM create blob failed errno=%d\n", errno);
        doom_drm_destroy();
        return false;
    }

    DoomDRM.connector_id = connector_id;
    DoomDRM.crtc_id = crtc_id;
    DoomDRM.plane_id = plane_id;
    DoomDRM.mode = mode;
    DoomDRM.dumb_handle = dumb.handle;
    DoomDRM.dumb_pitch = dumb.pitch;
    DoomDRM.dumb_size = dumb.size;
    DoomDRM.dmabuf_fd = prime.fd;
    DoomDRM.frame_map = (uint8_t*) map;
    DoomDRM.mode_blob_id = blob.blob_id;

    memset(&DoomDRM.atomic_req, 0, sizeof(DoomDRM.atomic_req));
    DoomDRM.atomic_req.connector_id = DoomDRM.connector_id;
    DoomDRM.atomic_req.crtc_id = DoomDRM.crtc_id;
    DoomDRM.atomic_req.plane_id = DoomDRM.plane_id;
    DoomDRM.atomic_req.fb_handle = DoomDRM.dumb_handle;
    DoomDRM.atomic_req.mode_blob_id = DoomDRM.mode_blob_id;
    DoomDRM.atomic_req.active = 1U;
    DoomDRM.atomic_req.src_x = 0U;
    DoomDRM.atomic_req.src_y = 0U;
    DoomDRM.atomic_req.src_w = ((uint32_t) SCREENWIDTH) << 16;
    DoomDRM.atomic_req.src_h = ((uint32_t) SCREENHEIGHT) << 16;
    DoomDRM.atomic_req.crtc_x = 0;
    DoomDRM.atomic_req.crtc_y = 0;
    DoomDRM.atomic_req.crtc_w = DoomDRM.mode.hdisplay;
    DoomDRM.atomic_req.crtc_h = DoomDRM.mode.vdisplay;

    DoomDRM.ready = true;
    printf("[DOOM] DRM ready mode=%ux%u source=%ux%u\n",
           (unsigned int) DoomDRM.mode.hdisplay,
           (unsigned int) DoomDRM.mode.vdisplay,
           (unsigned int) SCREENWIDTH,
           (unsigned int) SCREENHEIGHT);
    return true;
}

void I_SetPalette(byte* palette)
{
    if (!palette)
        return;

    memcpy(DoomDRM.palette, palette, sizeof(DoomDRM.palette));
}

void I_UpdateNoBlit(void)
{
}

void I_InitGraphics(void)
{
    (void) doom_drm_init();
}

void I_StartTic(void)
{
    doom_poll_keyboard();
}

void I_ReadScreen(byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_StartFrame(void)
{
}

void I_ShutdownGraphics(void)
{
    doom_drm_destroy();
}

void I_FinishUpdate(void)
{
    if (!DoomDRM.ready || !DoomDRM.frame_map)
        return;

    for (uint32_t y = 0; y < (uint32_t) SCREENHEIGHT; y++)
    {
        const uint8_t* src = &screens[0][y * SCREENWIDTH];
        uint8_t* dst_row = DoomDRM.frame_map + ((size_t) y * DoomDRM.dumb_pitch);
        uint32_t* dst = (uint32_t*) dst_row;

        for (uint32_t x = 0; x < (uint32_t) SCREENWIDTH; x++)
        {
            uint8_t idx = src[x];
            uint8_t r = DoomDRM.palette[(size_t) idx * 3U + 0U];
            uint8_t g = DoomDRM.palette[(size_t) idx * 3U + 1U];
            uint8_t b = DoomDRM.palette[(size_t) idx * 3U + 2U];
            dst[x] = ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }
    }

    if (ioctl(DoomDRM.card_fd, DRM_IOCTL_MODE_ATOMIC, &DoomDRM.atomic_req) < 0)
    {
        printf("[DOOM] DRM atomic commit failed errno=%d\n", errno);
        DoomDRM.ready = false;
    }
}
