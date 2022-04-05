#ifndef _PPM_H
#define _PMM_H

#include <stdint.h>

#define PHYS_PAGE_SIZE          4096

#define PMM_MEM_NOTAVALIABLE    0

#define AHCI_SIZE               122880

typedef struct PMM_region
{
    uint64_t addr_start;
    uint64_t addr_end;
    uint64_t addr_mmap_start;
    uint64_t len;
} PMM_region_t;

void PMM_init(uint64_t kernel_start, uint64_t kernel_end);

uint64_t PMM_get_kernel_start(void);
uint64_t PMM_get_kernel_end(void);
uint64_t PMM_get_max_phys(void);

void PMM_init_region(uint64_t addr, uint64_t len);

void PMM_mmap_unset(PMM_region_t* region, int bit);
void PMM_mmap_set(PMM_region_t* region, int bit);

uint32_t PMM_get_index_of_free_page(PMM_region_t region);
void* PMM_alloc_page(void);

#endif