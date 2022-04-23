#ifndef _PPM_H
#define _PMM_H

#include <stdint.h>

#define PHYS_PAGE_SIZE          4096

#define PMM_MEM_NOTAVALIABLE    0

#define AHCI_SIZE               122880

typedef struct PMM_region
{
    uintptr_t addr_start;
    uintptr_t addr_end;
    uintptr_t addr_mmap_start;
    uint64_t len;
} PMM_region_t;

void PMM_init(uintptr_t kernel_start, uintptr_t kernel_end);

uintptr_t PMM_get_kernel_start(void);
uintptr_t PMM_get_kernel_end(void);
uintptr_t PMM_get_AHCI_phys(void);

PMM_region_t* PMM_get_regions(void);
int PMM_get_num_regions(void);

void PMM_init_region(uintptr_t addr, uintptr_t len);

void PMM_mmap_unset(PMM_region_t* region, int bit);
void PMM_mmap_set(PMM_region_t* region, int bit);

uint32_t PMM_get_index_of_free_page(PMM_region_t region);
void* PMM_alloc_page(void);
void PMM_dealloc_page(void* ptr);

void PMM_init_AHCI(void);

#endif