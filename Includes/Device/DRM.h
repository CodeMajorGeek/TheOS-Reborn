#ifndef _DRM_H
#define _DRM_H

#include <Debug/Spinlock.h>
#include <UAPI/DRM.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRM_CONNECTOR_ID 101U
#define DRM_CRTC_ID      201U
#define DRM_PLANE_ID     301U

#define DRM_MAX_FILES            32U
#define DRM_MAX_FILE_HANDLES     256U
#define DRM_MAX_BUFFERS          128U
#define DRM_MAX_MODE_BLOBS       128U
#define DRM_MAX_CONNECTOR_MODES  128U
#define DRM_MAX_PAGES_PER_BUFFER 8192U

#define DRM_BOCHS_DISPI_INDEX_PORT  0x01CEU
#define DRM_BOCHS_DISPI_DATA_PORT   0x01CFU
#define DRM_BOCHS_DISPI_REG_ID      0x0U
#define DRM_BOCHS_DISPI_REG_XRES    0x1U
#define DRM_BOCHS_DISPI_REG_YRES    0x2U
#define DRM_BOCHS_DISPI_REG_BPP     0x3U
#define DRM_BOCHS_DISPI_REG_ENABLE  0x4U
#define DRM_BOCHS_DISPI_REG_BANK    0x5U
#define DRM_BOCHS_DISPI_REG_VIRT_W  0x6U
#define DRM_BOCHS_DISPI_REG_VIRT_H  0x7U
#define DRM_BOCHS_DISPI_REG_XOFF    0x8U
#define DRM_BOCHS_DISPI_REG_YOFF    0x9U

#define DRM_BOCHS_DISPI_ID0 0xB0C0U
#define DRM_BOCHS_DISPI_ID1 0xB0C1U
#define DRM_BOCHS_DISPI_ID2 0xB0C2U
#define DRM_BOCHS_DISPI_ID3 0xB0C3U
#define DRM_BOCHS_DISPI_ID4 0xB0C4U
#define DRM_BOCHS_DISPI_ID5 0xB0C5U

#define DRM_BOCHS_DISPI_DISABLED      0x00U
#define DRM_BOCHS_DISPI_ENABLED       0x01U
#define DRM_BOCHS_DISPI_LFB_ENABLED   0x40U
#define DRM_BOCHS_DISPI_NOCLEARMEM    0x80U

typedef struct drm_file_handle
{
    bool used;
    uint32_t handle;
    uint32_t buffer_id;
} drm_file_handle_t;

typedef struct drm_file
{
    bool used;
    uint32_t owner_pid;
    uint32_t file_id;
    uint32_t next_handle;
    drm_file_handle_t handles[DRM_MAX_FILE_HANDLES];
} drm_file_t;

typedef struct drm_buffer
{
    bool used;
    uint32_t buffer_id;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t format;
    uint32_t pitch;
    uint32_t page_count;
    uint64_t size;
    uintptr_t* page_phys;
    uint32_t handle_refs;
    uint32_t fd_refs;
    uint32_t map_page_refs;
} drm_buffer_t;

typedef struct drm_mode_blob
{
    bool used;
    uint32_t blob_id;
    uint32_t length;
    drm_mode_modeinfo_t mode;
} drm_mode_blob_t;

typedef struct drm_crtc_state
{
    uint32_t fb_handle;
    uint32_t fb_buffer_id;
    uint32_t owner_file_id;
    bool active;
    bool mode_valid;
    drm_mode_modeinfo_t mode;
} drm_crtc_state_t;

typedef struct drm_plane_state
{
    uint32_t fb_handle;
    uint32_t fb_buffer_id;
    uint32_t crtc_id;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
    int32_t crtc_x;
    int32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
} drm_plane_state_t;

typedef struct drm_connector_state
{
    uint32_t crtc_id;
} drm_connector_state_t;

typedef struct drm_runtime_state
{
    bool ready;
    bool tty_disabled;
    uint32_t next_file_id;
    uint32_t next_buffer_id;
    uint32_t next_blob_id;
    uint32_t master_file_id;
    spinlock_t lock;
    bool lock_ready;
    drm_file_t files[DRM_MAX_FILES];
    drm_buffer_t buffers[DRM_MAX_BUFFERS];
    drm_mode_blob_t blobs[DRM_MAX_MODE_BLOBS];
    drm_crtc_state_t crtc;
    drm_plane_state_t plane;
    drm_connector_state_t connector;
    drm_mode_modeinfo_t preferred_mode;
    drm_mode_modeinfo_t connector_modes[DRM_MAX_CONNECTOR_MODES];
    uint32_t connector_mode_count;
    bool bochs_probe_done;
    bool bochs_available;
    uint16_t bochs_id;
} drm_runtime_state_t;

void DRM_init(void);
bool DRM_is_available(void);

bool DRM_open_file(uint32_t owner_pid, uint32_t* out_file_id);
void DRM_close_file(uint32_t file_id);
bool DRM_set_master(uint32_t file_id);
bool DRM_drop_master(uint32_t file_id);
bool DRM_is_master(uint32_t file_id);

bool DRM_get_resources(uint32_t file_id, drm_mode_get_resources_t* io);
bool DRM_get_connector(uint32_t file_id,
                       drm_mode_get_connector_t* io,
                       drm_mode_modeinfo_t* out_modes,
                       uint32_t modes_capacity,
                       uint32_t* out_modes_written);
bool DRM_get_crtc(uint32_t file_id, drm_mode_get_crtc_t* io);
bool DRM_get_plane(uint32_t file_id,
                   drm_mode_get_plane_t* io,
                   uint32_t* out_formats,
                   uint32_t formats_capacity,
                   uint32_t* out_formats_written);

bool DRM_create_dumb(uint32_t file_id, drm_mode_create_dumb_t* io);
bool DRM_destroy_dumb(uint32_t file_id, uint32_t handle);

bool DRM_create_mode_blob(uint32_t file_id, const void* data, uint32_t length, uint32_t* out_blob_id);
bool DRM_destroy_mode_blob(uint32_t file_id, uint32_t blob_id);

bool DRM_prime_handle_to_dmabuf(uint32_t file_id, uint32_t handle, uint32_t* out_dmabuf_id);
bool DRM_prime_dmabuf_to_handle(uint32_t file_id, uint32_t dmabuf_id, uint32_t* out_handle);

bool DRM_atomic_commit(uint32_t file_id, const drm_mode_atomic_req_t* req);

bool DRM_dmabuf_ref_fd(uint32_t dmabuf_id);
void DRM_dmabuf_unref_fd(uint32_t dmabuf_id);
bool DRM_dmabuf_get_layout(uint32_t dmabuf_id, uint64_t* out_size, uint32_t* out_page_count);
bool DRM_dmabuf_get_page_phys(uint32_t dmabuf_id, uint32_t page_index, uintptr_t* out_phys);
bool DRM_dmabuf_ref_map_pages(uint32_t dmabuf_id, uint32_t page_count);
bool DRM_dmabuf_ref_map_pages_by_phys(uintptr_t phys, uint32_t page_count);
bool DRM_dmabuf_unref_map_pages_by_phys(uintptr_t phys, uint32_t page_count);
bool DRM_dmabuf_contains_phys(uintptr_t phys, uint32_t* out_dmabuf_id);

#endif
