#include <Memory/PMM.h>

#include <Memory/KMem.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PMM_MAX_REGIONS 64

static PMM_region_t PMM_regions_store[PMM_MAX_REGIONS];
static PMM_region_t* PMM_regions = PMM_regions_store;

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
    uintptr_t region_start = addr;
    uintptr_t region_end = addr + len;

    // Never manage legacy low memory (IVT/BIOS/EBDA/etc.) with PMM/KMEM.
    if (region_end <= 0x100000)
        return;
    if (region_start < 0x100000)
        region_start = 0x100000;
    if (region_end <= region_start)
        return;

    PMM_region_t region;
    region.addr_start = region_start;
    region.addr_end = region_end;
    region.addr_mmap_start = (uintptr_t) -1;
    region.len = region_end - region_start;

    size_t num_pages = region.len / PHYS_PAGE_SIZE;
    size_t mmap_size = num_pages < (sizeof (uint64_t) * 8) ? 1 : num_pages / (sizeof (uint64_t) * 8) + (num_pages % (sizeof (uint64_t) * 8) == 0 ? 0 : 1);
    uint64_t mmap_num_pages = mmap_size < PHYS_PAGE_SIZE ? 1 :
        (mmap_size % PHYS_PAGE_SIZE == 0 ? mmap_size : (mmap_size / PHYS_PAGE_SIZE) + 1);
    uintptr_t kmem_reserved_start = 0;
    uintptr_t kmem_reserved_end = 0;

    for (uint64_t i = region.addr_start; i < region.addr_end; i += PHYS_PAGE_SIZE)
    {
        if (i >= PMM_kernel_start && i < PMM_kernel_end)
            continue; // Kernel pages !
        else if (region.addr_mmap_start == (uintptr_t) -1)
        {
            region.addr_mmap_start = i;
            memset((void*) region.addr_mmap_start, PMM_MEM_NOTAVALIABLE, mmap_num_pages * PHYS_PAGE_SIZE);
            i += mmap_num_pages * PHYS_PAGE_SIZE;
            if (!is_kmem_initialized)
            {
                uintptr_t heap_available = (region.addr_end > i) ? (region.addr_end - i) : 0;
                size_t heap_size = (size_t) (heap_available & ~(uintptr_t) (PHYS_PAGE_SIZE - 1));
                if (heap_size > KMEM_HEAP_SIZE)
                    heap_size = KMEM_HEAP_SIZE;

                if (heap_size > (sizeof (malloc_header_t) + sizeof (uintptr_t)))
                {
                    kmem_init(i, heap_size);
                    kmem_reserved_start = i;
                    kmem_reserved_end = kmem_reserved_start + heap_size;
                    is_kmem_initialized = TRUE;
                    i += heap_size;
                }
            }
            i -= PHYS_PAGE_SIZE;
            continue;
        }

        if (kmem_reserved_start != 0 && i >= kmem_reserved_start && i < kmem_reserved_end)
            continue;

        uint32_t bit = (uint32_t) ((i - region.addr_start) / PHYS_PAGE_SIZE);
        if (bit < num_pages)
            PMM_mmap_set(&region, (int) bit);
    }

    if (region.addr_mmap_start == (uintptr_t) -1)
        return;

    if (!is_kmem_initialized)
        panic("PMM: failed to initialize kernel heap");

    if (PMM_num_regions >= PMM_MAX_REGIONS)
        panic("PMM: too many memory regions");

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
    uint32_t num_pages = (uint32_t) (region.len / PHYS_PAGE_SIZE);
    uint32_t num_words = (num_pages + 63) / 64;
    uint64_t* bitmap = (uint64_t*) region.addr_mmap_start;

    for (uint32_t word_index = 0; word_index < num_words; word_index++)
    {
        uint64_t word = bitmap[word_index];
        if (word == 0)
            continue;

        for (uint8_t bit_index = 0; bit_index < 64; bit_index++)
        {
            uint64_t bit = 1ULL << bit_index;
            if ((word & bit) == 0)
                continue;

            uint32_t page_index = word_index * 64 + bit_index;
            if (page_index < num_pages)
                return page_index;
        }
    }

    return (uint32_t) -1;
}

void* PMM_alloc_page(void)
{
    for (int i = 0; i < PMM_num_regions; i++)
    {
        PMM_region_t* region = &PMM_regions[i];
        while (TRUE)
        {
            uint32_t index = PMM_get_index_of_free_page(*region);
            if (index == (uint32_t) -1)
                break;

            uintptr_t page = region->addr_start + ((uintptr_t) index * PHYS_PAGE_SIZE);
            PMM_mmap_unset(region, (int) index);

            // Hard guard: never return low-memory or kernel image pages.
            // TODO : maybe find a clever way instead of hard guard.
            if (page < 0x100000 || (page >= PMM_kernel_start && page < PMM_kernel_end))
                continue;

            return (void*) page;
        }
    }

    return NULL; // Out of memory.
}

void PMM_dealloc_page(void* ptr)
{
    uint64_t addr = (uint64_t) ptr;
    // TODO: method to get the region of a page.
    
}
