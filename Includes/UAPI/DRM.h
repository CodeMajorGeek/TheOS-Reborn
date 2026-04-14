#ifndef _UAPI_DRM_H
#define _UAPI_DRM_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

#define DRM_NODE_PATH "/dev/dri/card0"

#define DRM_OBJECT_CONNECTOR 1U
#define DRM_OBJECT_CRTC      2U
#define DRM_OBJECT_PLANE     3U

#define DRM_MODE_CONNECTED   1U
#define DRM_MODE_DISCONNECTED 2U

#define DRM_PLANE_TYPE_PRIMARY 0U

/* Little-endian FOURCC for XRGB8888. */
#define DRM_FORMAT_XRGB8888 0x34325258U

#define DRM_MODE_ATOMIC_TEST_ONLY (1U << 0)
#define DRM_MODE_ATOMIC_NONBLOCK  (1U << 1)

#define DRM_FIXED_ONE (1U << 16)

#define DRM_CLOEXEC  (1U << 0)
#define DRM_RDWR     (1U << 1)

#define DRM_IOCTL_BASE                   0x6400U
#define DRM_IOCTL_MODE_GET_RESOURCES     (DRM_IOCTL_BASE + 0U)
#define DRM_IOCTL_MODE_GET_CONNECTOR     (DRM_IOCTL_BASE + 1U)
#define DRM_IOCTL_MODE_GET_CRTC          (DRM_IOCTL_BASE + 2U)
#define DRM_IOCTL_MODE_GET_PLANE         (DRM_IOCTL_BASE + 3U)
#define DRM_IOCTL_MODE_CREATE_DUMB       (DRM_IOCTL_BASE + 4U)
#define DRM_IOCTL_MODE_DESTROY_DUMB      (DRM_IOCTL_BASE + 5U)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD     (DRM_IOCTL_BASE + 6U)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE     (DRM_IOCTL_BASE + 7U)
#define DRM_IOCTL_MODE_CREATE_BLOB       (DRM_IOCTL_BASE + 8U)
#define DRM_IOCTL_MODE_DESTROY_BLOB      (DRM_IOCTL_BASE + 9U)
#define DRM_IOCTL_MODE_ATOMIC            (DRM_IOCTL_BASE + 10U)
#define DRM_IOCTL_SET_MASTER             (DRM_IOCTL_BASE + 11U)
#define DRM_IOCTL_DROP_MASTER            (DRM_IOCTL_BASE + 12U)

#ifndef __ASSEMBLER__
typedef struct drm_mode_modeinfo
{
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} drm_mode_modeinfo_t;

typedef struct drm_mode_get_resources
{
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t plane_id_ptr;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_planes;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
} drm_mode_get_resources_t;

typedef struct drm_mode_get_connector
{
    uint64_t modes_ptr;
    uint32_t connector_id;
    uint32_t count_modes;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t reserved;
} drm_mode_get_connector_t;

typedef struct drm_mode_get_crtc
{
    uint32_t crtc_id;
    uint32_t fb_handle;
    int32_t x;
    int32_t y;
    uint32_t active;
    uint32_t mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_get_crtc_t;

typedef struct drm_mode_get_plane
{
    uint64_t format_type_ptr;
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_handle;
    uint32_t possible_crtcs;
    uint32_t count_format_types;
    uint32_t plane_type;
    uint32_t reserved;
} drm_mode_get_plane_t;

typedef struct drm_mode_create_dumb
{
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} drm_mode_create_dumb_t;

typedef struct drm_mode_destroy_dumb
{
    uint32_t handle;
} drm_mode_destroy_dumb_t;

typedef struct drm_prime_handle
{
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
} drm_prime_handle_t;

typedef struct drm_mode_create_blob
{
    uint64_t data;
    uint32_t length;
    uint32_t blob_id;
} drm_mode_create_blob_t;

typedef struct drm_mode_destroy_blob
{
    uint32_t blob_id;
} drm_mode_destroy_blob_t;

typedef struct drm_mode_atomic_req
{
    uint32_t flags;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t fb_handle;
    uint32_t mode_blob_id;
    uint32_t active;
    uint32_t reserved0;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
    int32_t crtc_x;
    int32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
} drm_mode_atomic_req_t;
#endif

#endif
