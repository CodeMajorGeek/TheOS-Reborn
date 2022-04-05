#ifndef _VMM_H
#define _VMM_H

#include <stdint.h>

#define PRESENT     1 << 0   
#define WRITABLE    1 << 1       
#define USER_MODE   1 << 2       

#define PML4_INDEX(x)   (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x)   (((x) >> 30) & 0x1FF)
#define PDT_INDEX(x)    (((x) >> 21) & 0x1FF)
#define PT_INDEX(x)     (((x) >> 12) & 0x1FF)

#define FRAME           0xFFFFFFFFFFFFF000

#define IDENTITY_MAP_VIRTUAL_START  0xFFFFFFFFF0000000UL
#define IDENTITY_MAP_PHYSICAL_START 0x0UL

typedef struct PML4
{
    uint64_t entries[512];
} PML4_t;

typedef struct PDPT
{
    uint64_t entries[512];
} PDPT_t;

typedef struct PDT
{
    uint64_t entries[512];
} PDT_t;

typedef struct PT
{
    uint64_t entries[512];
} PT_t;

uint64_t VMM_get_AHCI_MMIO_virt(void);

void VMM_map_kernel(void);

void VMM_get_address();
void VMM_map_page(uint64_t virt, uint64_t phys);

void VMM_identity_mapping(void);

void VMM_load_cr3(void);

#endif