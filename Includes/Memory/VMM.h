#ifndef _VMM_H
#define _VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PRESENT     1 << 0   
#define WRITABLE    1 << 1       
#define USER_MODE   1 << 2       

#define VMM_RECURSIVE_INDEX 510

#define PML4_INDEX(x)   (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x)   (((x) >> 30) & 0x1FF)
#define PDT_INDEX(x)    (((x) >> 21) & 0x1FF)
#define PT_INDEX(x)     (((x) >> 12) & 0x1FF)

#define FRAME           0xFFFFFFFFFFFFF000

#define HILO2ADDR(hi, lo)   ((((uint64_t) hi) << 32) + lo)

#define ADDRHI(a)           ((a >> 32) & 0xFFFFFFFF)
#define ADDRLO(a)           (a & 0xFFFFFFFF)

#define V2P(a)              ((uintptr_t) a)
#define P2V(a)              ((uintptr_t) a)

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

uintptr_t VMM_get_AHCI_virt(void);

void VMM_map_kernel(void);
void VMM_map_userland_stack(void);
void VMM_identity_map_all(void);
void VMM_hardware_mapping(void);

void VMM_map_page(uintptr_t virt, uintptr_t phys);
void VMM_map_pages(uintptr_t virt, uintptr_t phys, size_t len);

void VMM_load_cr3(void);

bool VMM_virt_to_phys(uintptr_t virt, uintptr_t* phys_out);
bool VMM_phys_to_virt(uintptr_t phys, uintptr_t* virt_out);
bool VMM_phys_to_virt_identity(uintptr_t phys, uintptr_t* virt_out);

#endif
