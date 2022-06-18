#include <Memory/PMM.h>

#include <Memory/KMem.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

static PMM_region_t* PMM_regions = (PMM_region_t*) -1;

static int PMM_num_regions = 0;

static uintptr_t PMM_kernel_start;
static uintptr_t PMM_kernel_end;

static bool is_kmem_initialized = FALSE;

void PMM_init(uintptr_t kernel_start, uintptr_t kernel_end)
{
    PMM_kernel_start = kernel_start;
    PMM_kernel_end = kernel_end;
}

uintptr_t PMM_get_kernel_start(void)
{
    return PMM_kernel_start;
}

uintptr_t PMM_get_kernel_end(void)
{
    return PMM_kernel_end;
}

PMM_region_t* PMM_get_regions(void)
{
    return PMM_regions;
}

int PMM_get_num_regions(void)
{
    return PMM_num_regions;
}

void PMM_init_region(uintptr_t addr, uintptr_t len)
{
    PMM_region_t region;
    region.addr_start = addr;
    region.addr_end = addr + len;
    region.addr_mmap_start = -1;
    region.len = len - sizeof (PMM_region_t);

    size_t num_pages = region.len / PHYS_PAGE_SIZE;
    size_t mmap_size = num_pages < (sizeof (uint64_t) * 8) ? 1 : num_pages / (sizeof (uint64_t) * 8) + (num_pages % (sizeof (uint64_t) * 8) == 0 ? 0 : 1);
    uint64_t mmap_num_pages = mmap_size < PHYS_PAGE_SIZE ? 1 :
        (mmap_size % PHYS_PAGE_SIZE == 0 ? mmap_size : (mmap_size / PHYS_PAGE_SIZE) + 1);

    for (uint64_t i = region.addr_start; i < region.addr_end; i += PHYS_PAGE_SIZE)
    {
        if (i <= PMM_kernel_end && i >= PMM_kernel_start)
            continue; // Kernel pages !
        else if (region.addr_mmap_start == -1)
        {
            region.addr_mmap_start = i;
            memsetq((void*) region.addr_mmap_start, PMM_MEM_NOTAVALIABLE, mmap_num_pages * PHYS_PAGE_SIZE);
            i += mmap_num_pages * PHYS_PAGE_SIZE;
            if (!is_kmem_initialized)
            {
                kmem_init(i);
                is_kmem_initialized = TRUE;
            }
        }
        else
            PMM_mmap_set(&region, i / PHYS_PAGE_SIZE);
    }

    if (PMM_regions == (PMM_region_t*) -1)
        PMM_regions = kmalloc(sizeof (PMM_region_t));
    else
        PMM_regions = krealloc(PMM_regions, sizeof (PMM_region_t));
    
    memcpy(&PMM_regions[PMM_num_regions++], &region, sizeof (region)); // Store the current region.
}

void PMM_mmap_unset(PMM_region_t* region, int bit)
{
    ((uint64_t*) region->addr_mmap_start)[bit / 64] &= ~(1ULL << (bit % 64));
}

void PMM_mmap_set(PMM_region_t* region, int bit)
{
    ((uint64_t*) region->addr_mmap_start)[bit / 64] |= (1ULL << (bit % 64));
}

uint32_t PMM_get_index_of_free_page(PMM_region_t region)
{   
    for (uint64_t i = 0; i < region.len / PHYS_PAGE_SIZE; i++)
    {
        for (uint8_t k = 0; k < sizeof (uint64_t) * 8; k++)
        {
            uint64_t bit = 1ULL << k;
            if (((uint64_t*) region.addr_mmap_start)[i] & bit)
            {
                return i * sizeof (uint64_t) * 8 + k;
            }
        }
    }

    return -1;
}

void* PMM_alloc_page(void)
{
    uint32_t index = -1;
    PMM_region_t region;

    size_t i = 0;
    for (i = 0; i < PMM_num_regions; i++)
    {
        region = PMM_regions[i];
        index = PMM_get_index_of_free_page(region);

        if (index != -1)
            break;
    }

    if (index == -1)
        return NULL; // Out of memory.

    PMM_mmap_unset(&region, index);
    return (void*) (region.addr_start + (index * PHYS_PAGE_SIZE));
}

void PMM_dealloc_page(void* ptr)
{
    uint64_t addr = (uint64_t) ptr;
    // TODO: method to get the region of a page.
    
}