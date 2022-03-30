#ifndef _PPM_H
#define _PMM_H

#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

#define PMM_PHYS_PAGE_SIZE      4096

#define PMM_MEM_NOTAVALIABLE    0

#define PMM_AHCI_SIZE           122880

typedef struct PMM_region
{
    uint64_t addr_start;
    uint64_t addr_end;
    uint64_t len;
} PMM_region_t;

typedef struct PMM_region_alloc
{
    PMM_region_t* region;
    uint32_t index; 
} PMM_region_alloc_t;

void PMM_init(uint64_t kernel_start, uint64_t kernel_end);

void PMM_init_region(uint64_t addr, uint64_t len);

void PMM_mmap_unset(PMM_region_t* region, int bit);
void PMM_mmap_set(PMM_region_t* region, int bit);

PMM_region_alloc_t PMM_get_index_of_free_page(void);
void* PMM_alloc_page(void);

#endif