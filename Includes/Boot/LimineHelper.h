#ifndef _BOOT_LIMINE_HELPER_H
#define _BOOT_LIMINE_HELPER_H

#include <Device/TTY.h>

#include <stdbool.h>
#include <stdint.h>

#define LIMINE_HELPER_CMDLINE_SIZE 256U

typedef struct LimineHelper_runtime_state
{
    TTY_framebuffer_info_t boot_framebuffer;
    bool boot_framebuffer_available;
    char boot_cmdline[LIMINE_HELPER_CMDLINE_SIZE];
    uint64_t boot_hhdm_offset;
    uintptr_t boot_rsdp_addr;
    bool boot_mbr_disk_id_hint_present;
    uint32_t boot_mbr_disk_id_hint;
    bool boot_slice_hint_present;
    int32_t boot_slice_hint;
    bool bootloader_reclaimable_promoted;
} LimineHelper_runtime_state_t;

bool LimineHelper_base_revision_supported(void);

void LimineHelper_resolve_runtime_kernel_bases(uintptr_t linked_phys_start,
                                                uintptr_t linked_phys_end,
                                                uintptr_t linked_virt_start,
                                                uintptr_t linked_virt_end,
                                                uintptr_t* runtime_phys_start_out,
                                                uintptr_t* runtime_phys_end_out,
                                                uintptr_t* runtime_virt_start_out,
                                                uintptr_t* runtime_virt_end_out);

void LimineHelper_read_boot_info(void);
bool LimineHelper_init_acpi_from_rsdp_if_needed(void);
void LimineHelper_promote_bootloader_reclaimable(void);

bool LimineHelper_get_framebuffer(TTY_framebuffer_info_t* out_info);
bool LimineHelper_get_mbr_disk_id_hint(uint32_t* out_disk_id);
bool LimineHelper_get_slice_hint(int32_t* out_slice_hint);

#endif
