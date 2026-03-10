#include <Boot/LimineHelper.h>
#include <Boot/Limine.h>

#include <CPU/ACPI.h>
#include <Debug/KDebug.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>

#include <stdio.h>
#include <string.h>

extern volatile uint64_t limine_base_revision[];
extern volatile struct limine_executable_address_request limine_executable_address_request;
extern volatile struct limine_executable_cmdline_request limine_cmdline_request;
extern volatile struct limine_executable_file_request limine_executable_file_request;
extern volatile struct limine_memmap_request limine_memmap_request;
extern volatile struct limine_hhdm_request limine_hhdm_request;
extern volatile struct limine_framebuffer_request limine_framebuffer_request;
extern volatile struct limine_rsdp_request limine_rsdp_request;

static TTY_framebuffer_info_t limine_boot_framebuffer = { 0 };
static bool limine_boot_framebuffer_available = false;
static char limine_boot_cmdline[256] = { 0 };
static uint64_t limine_boot_hhdm_offset = 0;
static uintptr_t limine_boot_rsdp_addr = 0;
static bool limine_boot_mbr_disk_id_hint_present = false;
static uint32_t limine_boot_mbr_disk_id_hint = 0;
static bool limine_boot_slice_hint_present = false;
static int32_t limine_boot_slice_hint = -1;

static uintptr_t limine_addr_to_phys(uintptr_t addr)
{
    uintptr_t hhdm_base = VMM_get_hhdm_base();
    if (addr >= hhdm_base)
        return addr - hhdm_base;

    return addr;
}

static uint64_t limine_resolution_score(uint64_t width, uint64_t height, uint64_t bpp)
{
    uint64_t pixels = 0;
    if (width != 0 && height != 0 && width <= (UINT64_MAX / height))
        pixels = width * height;

    if (pixels > (UINT64_MAX >> 8))
        return UINT64_MAX;

    return (pixels << 8) | (bpp & 0xFFU);
}

static bool limine_video_mode_is_valid(const struct limine_video_mode* mode)
{
    if (!mode)
        return false;

    if (mode->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        mode->width == 0 || mode->height == 0 || mode->pitch == 0 || mode->bpp == 0)
        return false;

    if (mode->width > UINT32_MAX || mode->height > UINT32_MAX || mode->pitch > UINT32_MAX ||
        mode->bpp > UINT8_MAX)
        return false;

    return true;
}

static bool limine_framebuffer_is_valid(const struct limine_framebuffer* fb)
{
    if (!fb)
        return false;

    if (!fb->address ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        fb->width == 0 || fb->height == 0 || fb->pitch == 0 || fb->bpp == 0)
        return false;

    if (fb->width > UINT32_MAX || fb->height > UINT32_MAX || fb->pitch > UINT32_MAX ||
        fb->bpp > UINT8_MAX)
        return false;

    return true;
}

static uint64_t limine_framebuffer_best_mode_score(const struct limine_framebuffer* fb,
                                                   const struct limine_video_mode** out_mode)
{
    const struct limine_video_mode* best_mode = NULL;
    uint64_t best_score = 0;

    if (fb->mode_count > 0 && fb->modes)
    {
        for (uint64_t i = 0; i < fb->mode_count; i++)
        {
            const struct limine_video_mode* mode = fb->modes[i];
            if (!limine_video_mode_is_valid(mode))
                continue;

            uint64_t score = limine_resolution_score(mode->width, mode->height, mode->bpp);
            if (score <= best_score)
                continue;

            best_score = score;
            best_mode = mode;
        }
    }

    if (out_mode)
        *out_mode = best_mode;

    if (best_score != 0)
        return best_score;

    return limine_resolution_score(fb->width, fb->height, fb->bpp);
}

static const char* limine_memmap_type_name(uint64_t type)
{
    switch (type)
    {
        case LIMINE_MEMMAP_USABLE:
            return "usable";
        case LIMINE_MEMMAP_RESERVED:
            return "reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return "acpi-reclaim";
        case LIMINE_MEMMAP_ACPI_NVS:
            return "acpi-nvs";
        case LIMINE_MEMMAP_BAD_MEMORY:
            return "bad";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "boot-reclaim";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return "kernel/modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return "framebuffer";
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            return "reserved-mapped";
        default:
            return "unknown";
    }
}

static bool limine_memmap_is_pmm_allocatable(uint64_t type)
{
    return type == LIMINE_MEMMAP_USABLE;
}

static bool limine_memmap_should_map_hhdm(uint64_t type)
{
    switch (type)
    {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_FRAMEBUFFER:
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            return true;
        default:
            return false;
    }
}

bool LimineHelper_base_revision_supported(void)
{
    return LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) != false;
}

void LimineHelper_resolve_runtime_kernel_bases(uintptr_t linked_phys_start,
                                                uintptr_t linked_phys_end,
                                                uintptr_t linked_virt_start,
                                                uintptr_t linked_virt_end,
                                                uintptr_t* runtime_phys_start_out,
                                                uintptr_t* runtime_phys_end_out,
                                                uintptr_t* runtime_virt_start_out,
                                                uintptr_t* runtime_virt_end_out)
{
    if (!runtime_phys_start_out || !runtime_phys_end_out || !runtime_virt_start_out || !runtime_virt_end_out)
        return;

    *runtime_phys_start_out = linked_phys_start;
    *runtime_phys_end_out = linked_phys_end;
    *runtime_virt_start_out = linked_virt_start;
    *runtime_virt_end_out = linked_virt_end;

    if (limine_executable_address_request.response)
    {
        uintptr_t limine_phys_base = (uintptr_t) limine_executable_address_request.response->physical_base;
        uintptr_t limine_virt_base = (uintptr_t) limine_executable_address_request.response->virtual_base;
        uintptr_t linked_phys_span = linked_phys_end - linked_phys_start;
        uintptr_t linked_virt_span = linked_virt_end - linked_virt_start;

        *runtime_phys_start_out = limine_phys_base;
        *runtime_phys_end_out = limine_phys_base + linked_phys_span;
        *runtime_virt_start_out = limine_virt_base;
        *runtime_virt_end_out = limine_virt_base + linked_virt_span;
        return;
    }

    kdebug_puts("[BOOT] Limine executable address response missing, using linked bases\n");
}

void LimineHelper_read_boot_info(void)
{
    if (limine_hhdm_request.response)
    {
        limine_boot_hhdm_offset = limine_hhdm_request.response->offset;
        VMM_set_hhdm_base((uintptr_t) limine_boot_hhdm_offset);
        kdebug_printf("[BOOT] Limine HHDM offset=0x%llX\n",
                      (unsigned long long) limine_boot_hhdm_offset);
    }
    else
    {
        limine_boot_hhdm_offset = (uint64_t) VMM_get_hhdm_base();
        kdebug_printf("[BOOT] Limine HHDM unavailable, fallback offset=0x%llX\n",
                      (unsigned long long) limine_boot_hhdm_offset);
    }

    if (limine_cmdline_request.response && limine_cmdline_request.response->cmdline)
    {
        const char* cmdline = limine_cmdline_request.response->cmdline;
        size_t len = strlen(cmdline);
        if (len >= sizeof(limine_boot_cmdline))
            len = sizeof(limine_boot_cmdline) - 1U;
        memcpy(limine_boot_cmdline, cmdline, len);
        limine_boot_cmdline[len] = '\0';

        kdebug_printf("[BOOT] Limine cmdline: %s\n", limine_boot_cmdline);
    }
    else
    {
        limine_boot_cmdline[0] = '\0';
        kdebug_puts("[BOOT] Limine cmdline unavailable\n");
    }

    PMM_boot_entries_reset();
    if (limine_memmap_request.response)
    {
        uint64_t pmm_alloc_regions = 0;
        for (uint64_t i = 0; i < limine_memmap_request.response->entry_count; i++)
        {
            struct limine_memmap_entry* entry = limine_memmap_request.response->entries[i];
            if (!entry)
                continue;

            bool allocatable = limine_memmap_is_pmm_allocatable(entry->type);
            bool map_hhdm = limine_memmap_should_map_hhdm(entry->type);

            if (!PMM_boot_entry_add((uintptr_t) entry->base,
                                    (uintptr_t) entry->length,
                                    entry->type,
                                    allocatable,
                                    map_hhdm))
            {
                kdebug_printf("[BOOT] Limine memmap[%llu] drop type=%s base=0x%llX len=0x%llX\n",
                              (unsigned long long) i,
                              limine_memmap_type_name(entry->type),
                              (unsigned long long) entry->base,
                              (unsigned long long) entry->length);
                continue;
            }

            kdebug_printf("[BOOT] Limine memmap[%llu] type=%s base=0x%llX len=0x%llX alloc=%s hhdm=%s\n",
                          (unsigned long long) i,
                          limine_memmap_type_name(entry->type),
                          (unsigned long long) entry->base,
                          (unsigned long long) entry->length,
                          allocatable ? "yes" : "no",
                          map_hhdm ? "yes" : "no");

            if (allocatable)
            {
                PMM_init_region((uintptr_t) entry->base, (uintptr_t) entry->length);
                pmm_alloc_regions++;
            }
        }

        kdebug_printf("[BOOT] Limine memmap parsed entries=%llu pmm_alloc_regions=%llu boot_entries=%d\n",
                      (unsigned long long) limine_memmap_request.response->entry_count,
                      (unsigned long long) pmm_alloc_regions,
                      PMM_get_boot_entry_count());
    }
    else
    {
        kdebug_puts("[BOOT] Limine memmap unavailable\n");
    }

    if (limine_rsdp_request.response && limine_rsdp_request.response->address)
    {
        uintptr_t rsdp_raw = (uintptr_t) limine_rsdp_request.response->address;
        limine_boot_rsdp_addr = limine_addr_to_phys(rsdp_raw);
        kdebug_printf("[BOOT] Limine RSDP deferred raw=0x%llX phys=0x%llX\n",
                      (unsigned long long) rsdp_raw,
                      (unsigned long long) limine_boot_rsdp_addr);
    }

    limine_boot_mbr_disk_id_hint_present = false;
    limine_boot_mbr_disk_id_hint = 0;
    limine_boot_slice_hint_present = false;
    limine_boot_slice_hint = -1;

    if (limine_executable_file_request.response &&
        limine_executable_file_request.response->executable_file)
    {
        struct limine_file* executable = limine_executable_file_request.response->executable_file;
        if (executable->mbr_disk_id != 0)
        {
            limine_boot_mbr_disk_id_hint_present = true;
            limine_boot_mbr_disk_id_hint = executable->mbr_disk_id;
        }
        if (executable->partition_index != 0)
        {
            limine_boot_slice_hint_present = true;
            limine_boot_slice_hint = (int32_t) executable->partition_index - 1;
        }

        kdebug_printf("[BOOT] Limine executable source media=%u mbr_disk_id=0x%X part_index=%u\n",
                      executable->media_type,
                      executable->mbr_disk_id,
                      executable->partition_index);
    }

    limine_boot_framebuffer_available = false;
    if (limine_framebuffer_request.response &&
        limine_framebuffer_request.response->framebuffer_count > 0 &&
        limine_framebuffer_request.response->framebuffers)
    {
        struct limine_framebuffer* primary_fb = NULL;
        uint64_t primary_fb_index = 0;
        uint64_t primary_score = 0;
        struct limine_framebuffer* best_fb = NULL;
        uint64_t best_fb_index = 0;
        uint64_t best_score = 0;

        for (uint64_t i = 0; i < limine_framebuffer_request.response->framebuffer_count; i++)
        {
            struct limine_framebuffer* fb = limine_framebuffer_request.response->framebuffers[i];
            if (!limine_framebuffer_is_valid(fb))
                continue;

            const struct limine_video_mode* best_mode = NULL;
            uint64_t score = limine_framebuffer_best_mode_score(fb, &best_mode);

            uint64_t mode_width = best_mode ? best_mode->width : fb->width;
            uint64_t mode_height = best_mode ? best_mode->height : fb->height;
            uint64_t mode_bpp = best_mode ? best_mode->bpp : fb->bpp;
            kdebug_printf("[BOOT] Limine fb[%llu]%s active=%llux%llu bpp=%u best=%llux%llu bpp=%llu modes=%llu\n",
                          (unsigned long long) i,
                          (i == 0) ? " PRIMARY" : "",
                          (unsigned long long) fb->width,
                          (unsigned long long) fb->height,
                          (unsigned int) fb->bpp,
                          (unsigned long long) mode_width,
                          (unsigned long long) mode_height,
                          (unsigned long long) mode_bpp,
                          (unsigned long long) fb->mode_count);

            if (i == 0 && score > primary_score)
            {
                primary_fb = fb;
                primary_fb_index = i;
                primary_score = score;
            }

            if (score > best_score)
            {
                best_fb = fb;
                best_fb_index = i;
                best_score = score;
            }
        }

        struct limine_framebuffer* selected_fb = primary_fb ? primary_fb : best_fb;
        uint64_t selected_index = primary_fb ? primary_fb_index : best_fb_index;
        if (selected_fb)
        {
            const struct limine_video_mode* selected_best_mode = NULL;
            uint64_t selected_score = limine_framebuffer_best_mode_score(selected_fb, &selected_best_mode);
            uint64_t selected_best_width = selected_best_mode ? selected_best_mode->width : selected_fb->width;
            uint64_t selected_best_height = selected_best_mode ? selected_best_mode->height : selected_fb->height;
            uint64_t selected_best_bpp = selected_best_mode ? selected_best_mode->bpp : selected_fb->bpp;

            kdebug_printf("[BOOT] Limine fb final source=%s fb[%llu] primary_valid=%s primary_score=0x%llX best_score=0x%llX selected_score=0x%llX\n",
                          primary_fb ? "PRIMARY" : "BEST",
                          (unsigned long long) selected_index,
                          primary_fb ? "yes" : "no",
                          (unsigned long long) primary_score,
                          (unsigned long long) best_score,
                          (unsigned long long) selected_score);
            kdebug_printf("[BOOT] Limine fb final active=%llux%llu bpp=%u pitch=%llu best=%llux%llu bpp=%llu\n",
                          (unsigned long long) selected_fb->width,
                          (unsigned long long) selected_fb->height,
                          (unsigned int) selected_fb->bpp,
                          (unsigned long long) selected_fb->pitch,
                          (unsigned long long) selected_best_width,
                          (unsigned long long) selected_best_height,
                          (unsigned long long) selected_best_bpp);

            uintptr_t fb_addr = (uintptr_t) selected_fb->address;
            limine_boot_framebuffer.phys_addr = limine_addr_to_phys(fb_addr);
            limine_boot_framebuffer.pitch = (uint32_t) selected_fb->pitch;
            limine_boot_framebuffer.width = (uint32_t) selected_fb->width;
            limine_boot_framebuffer.height = (uint32_t) selected_fb->height;
            limine_boot_framebuffer.bpp = (uint8_t) selected_fb->bpp;
            limine_boot_framebuffer.type = TTY_FRAMEBUFFER_TYPE_RGB;
            limine_boot_framebuffer.red_field_position = selected_fb->red_mask_shift;
            limine_boot_framebuffer.red_mask_size = selected_fb->red_mask_size;
            limine_boot_framebuffer.green_field_position = selected_fb->green_mask_shift;
            limine_boot_framebuffer.green_mask_size = selected_fb->green_mask_size;
            limine_boot_framebuffer.blue_field_position = selected_fb->blue_mask_shift;
            limine_boot_framebuffer.blue_mask_size = selected_fb->blue_mask_size;
            limine_boot_framebuffer_available = true;
            kdebug_printf("[BOOT] Limine framebuffer selected fb[%llu]%s raw=0x%llX phys=0x%llX %ux%u pitch=%u bpp=%u\n",
                          (unsigned long long) selected_index,
                          primary_fb ? " PRIMARY" : "",
                          (unsigned long long) fb_addr,
                          (unsigned long long) limine_boot_framebuffer.phys_addr,
                          (unsigned int) limine_boot_framebuffer.width,
                          (unsigned int) limine_boot_framebuffer.height,
                          (unsigned int) limine_boot_framebuffer.pitch,
                          (unsigned int) limine_boot_framebuffer.bpp);
        }
        else
        {
            kdebug_puts("[BOOT] Limine framebuffer unavailable or invalid\n");
        }
    }
    else
    {
        kdebug_puts("[BOOT] Limine framebuffer response unavailable\n");
    }
}

bool LimineHelper_init_acpi_from_rsdp_if_needed(void)
{
    if (limine_boot_rsdp_addr == 0)
        return false;

    uintptr_t rsdp_phys = limine_boot_rsdp_addr;
    uintptr_t rsdp_page_phys = rsdp_phys & FRAME;
    VMM_map_page(P2V(rsdp_page_phys), rsdp_page_phys);

    uintptr_t rsdp_end_phys = rsdp_phys + sizeof(ACPI_RSDP_descriptor20_t);
    if ((rsdp_end_phys - 1U) > rsdp_page_phys + (PHYS_PAGE_SIZE - 1U))
    {
        uintptr_t next_page_phys = rsdp_page_phys + PHYS_PAGE_SIZE;
        VMM_map_page(P2V(next_page_phys), next_page_phys);
    }

    uint8_t* rsdp = (uint8_t*) P2V(rsdp_phys);
    if (ACPI_RSDP_new_check(rsdp))
    {
        ACPI_init_XSDT((ACPI_RSDP_descriptor20_t*) rsdp);
        kdebug_printf("[BOOT] Limine ACPI init via HHDM RSDP phys=0x%llX virt=0x%llX (RSDP2)\n",
                      (unsigned long long) rsdp_phys,
                      (unsigned long long) (uintptr_t) rsdp);
        return true;
    }
    if (ACPI_RSDP_old_check(rsdp))
    {
        ACPI_init_RSDT((ACPI_RSDP_descriptor10_t*) rsdp);
        kdebug_printf("[BOOT] Limine ACPI init via HHDM RSDP phys=0x%llX virt=0x%llX (RSDP1)\n",
                      (unsigned long long) rsdp_phys,
                      (unsigned long long) (uintptr_t) rsdp);
        return true;
    }

    kdebug_printf("[BOOT] Limine ACPI init failed (RSDP phys=0x%llX virt=0x%llX)\n",
                  (unsigned long long) rsdp_phys,
                  (unsigned long long) (uintptr_t) rsdp);
    return false;
}

bool LimineHelper_get_framebuffer(TTY_framebuffer_info_t* out_info)
{
    if (!limine_boot_framebuffer_available || !out_info)
        return false;

    *out_info = limine_boot_framebuffer;
    return true;
}

bool LimineHelper_get_mbr_disk_id_hint(uint32_t* out_disk_id)
{
    if (!limine_boot_mbr_disk_id_hint_present || !out_disk_id)
        return false;

    *out_disk_id = limine_boot_mbr_disk_id_hint;
    return true;
}

bool LimineHelper_get_slice_hint(int32_t* out_slice_hint)
{
    if (!limine_boot_slice_hint_present || !out_slice_hint)
        return false;

    *out_slice_hint = limine_boot_slice_hint;
    return true;
}
