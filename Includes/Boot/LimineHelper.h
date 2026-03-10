#ifndef _BOOT_LIMINE_HELPER_H
#define _BOOT_LIMINE_HELPER_H

#include <Device/TTY.h>

#include <stdbool.h>
#include <stdint.h>

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

bool LimineHelper_get_framebuffer(TTY_framebuffer_info_t* out_info);
bool LimineHelper_get_mbr_disk_id_hint(uint32_t* out_disk_id);
bool LimineHelper_get_slice_hint(int32_t* out_slice_hint);

#endif
