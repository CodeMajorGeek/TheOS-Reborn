#ifndef _PPM_H
#define _PMM_H

#include <stdbool.h>
#include <stdint.h>

#define PHYS_PAGE_SIZE          4096
#define PMM_MEM_NOTAVALIABLE    0
#define PMM_MAX_REGIONS         64
#define PMM_MAX_BOOT_ENTRIES    256
#define PMM_BOOT_ENTRY_ALLOCATABLE (1U << 0)
#define PMM_BOOT_ENTRY_HHDM_MAP    (1U << 1)

typedef struct PMM_region
{
    uintptr_t addr_start;
    uintptr_t addr_end;
    uintptr_t addr_mmap_start;
    uint64_t len;
} PMM_region_t;

typedef struct PMM_boot_entry
{
    uintptr_t addr_start;
    uintptr_t addr_end;
    uint64_t type;
    uint8_t flags;
} PMM_boot_entry_t;

void PMM_init(uintptr_t kernel_phys_start,
              uintptr_t kernel_phys_end,
              uintptr_t kernel_virt_start,
              uintptr_t kernel_virt_end);

uintptr_t PMM_get_kernel_start(void);
uintptr_t PMM_get_kernel_end(void);
uintptr_t PMM_get_kernel_virt_start(void);
uintptr_t PMM_get_kernel_virt_end(void);

PMM_region_t* PMM_get_regions(void);
int PMM_get_num_regions(void);

void PMM_init_region(uintptr_t addr, uintptr_t len);

void PMM_boot_entries_reset(void);
bool PMM_boot_entry_add(uintptr_t addr,
                        uintptr_t len,
                        uint64_t type,
                        bool allocatable,
                        bool map_hhdm);
const PMM_boot_entry_t* PMM_get_boot_entries(void);
int PMM_get_boot_entry_count(void);

void PMM_mmap_unset(PMM_region_t* region, int bit);
void PMM_mmap_set(PMM_region_t* region, int bit);

uint32_t PMM_get_index_of_free_page(PMM_region_t region);
void* PMM_alloc_page(void);
void PMM_dealloc_page(void* ptr);

#endif
