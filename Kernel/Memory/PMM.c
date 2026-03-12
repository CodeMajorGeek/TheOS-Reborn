#include <Memory/PMM.h>

#include <Memory/KMem.h>
#include <Memory/VMM.h>

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static PMM_runtime_state_t PMM_state;

void PMM_init(uintptr_t kernel_phys_start,
              uintptr_t kernel_phys_end,
              uintptr_t kernel_virt_start,
              uintptr_t kernel_virt_end)
{
    PMM_state.kernel_phys_start = kernel_phys_start;
    PMM_state.kernel_phys_end = kernel_phys_end;
    PMM_state.kernel_virt_start = kernel_virt_start;
    PMM_state.kernel_virt_end = kernel_virt_end;
}

uintptr_t PMM_get_kernel_start(void)
{
    return PMM_state.kernel_phys_start;
}

uintptr_t PMM_get_kernel_end(void)
{
    return PMM_state.kernel_phys_end;
}

uintptr_t PMM_get_kernel_virt_start(void)
{
    return PMM_state.kernel_virt_start;
}

uintptr_t PMM_get_kernel_virt_end(void)
{
    return PMM_state.kernel_virt_end;
}

PMM_region_t* PMM_get_regions(void)
{
    return PMM_state.regions;
}

int PMM_get_num_regions(void)
{
    return PMM_state.num_regions;
}

void PMM_boot_entries_reset(void)
{
    PMM_state.boot_entry_count = 0;
    memset(PMM_state.boot_entries, 0, sizeof(PMM_state.boot_entries));
}

bool PMM_boot_entry_add(uintptr_t addr,
                        uintptr_t len,
                        uint64_t type,
                        bool allocatable,
                        bool map_hhdm)
{
    if (len == 0)
        return false;

    if (PMM_state.boot_entry_count >= PMM_MAX_BOOT_ENTRIES)
        return false;

    uintptr_t end = addr + len;
    if (end < addr)
        return false;

    uintptr_t aligned_start = addr & ~(uintptr_t) (PHYS_PAGE_SIZE - 1U);
    uintptr_t aligned_end = (end + (PHYS_PAGE_SIZE - 1U)) & ~(uintptr_t) (PHYS_PAGE_SIZE - 1U);
    if (aligned_end <= aligned_start)
        return false;

    PMM_boot_entry_t* entry = &PMM_state.boot_entries[PMM_state.boot_entry_count++];
    entry->addr_start = aligned_start;
    entry->addr_end = aligned_end;
    entry->type = type;
    entry->flags = 0;
    if (allocatable)
        entry->flags |= PMM_BOOT_ENTRY_ALLOCATABLE;
    if (map_hhdm)
        entry->flags |= PMM_BOOT_ENTRY_HHDM_MAP;

    return true;
}

const PMM_boot_entry_t* PMM_get_boot_entries(void)
{
    return PMM_state.boot_entries;
}

int PMM_get_boot_entry_count(void)
{
    return PMM_state.boot_entry_count;
}

bool PMM_promote_boot_entries_to_allocatable(uint64_t type,
                                             uint64_t* regions_added_out,
                                             uint64_t* pages_added_out)
{
    uint64_t regions_added = 0;
    uint64_t pages_added = 0;

    for (int i = 0; i < PMM_state.boot_entry_count; i++)
    {
        PMM_boot_entry_t* entry = &PMM_state.boot_entries[i];
        if (entry->type != type)
            continue;
        if ((entry->flags & PMM_BOOT_ENTRY_ALLOCATABLE) != 0)
            continue;

        uintptr_t len = entry->addr_end - entry->addr_start;
        int regions_before = PMM_state.num_regions;
        PMM_init_region(entry->addr_start, len);
        if (PMM_state.num_regions <= regions_before)
            continue;
        entry->flags |= PMM_BOOT_ENTRY_ALLOCATABLE;

        regions_added++;
        pages_added += (uint64_t) (PMM_state.regions[PMM_state.num_regions - 1].len / PHYS_PAGE_SIZE);
    }

    if (regions_added_out)
        *regions_added_out = regions_added;
    if (pages_added_out)
        *pages_added_out = pages_added;

    return regions_added != 0;
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
    size_t bitmap_words = (num_pages + ((sizeof(uint64_t) * 8U) - 1U)) / (sizeof(uint64_t) * 8U);
    size_t bitmap_bytes = bitmap_words * sizeof(uint64_t);
    size_t mmap_num_pages = (bitmap_bytes + (PHYS_PAGE_SIZE - 1U)) / PHYS_PAGE_SIZE;
    if (mmap_num_pages == 0)
        mmap_num_pages = 1;
    uintptr_t kmem_reserved_start = 0;
    uintptr_t kmem_reserved_end = 0;

    for (uint64_t i = region.addr_start; i < region.addr_end; i += PHYS_PAGE_SIZE)
    {
        if (i >= PMM_state.kernel_phys_start && i < PMM_state.kernel_phys_end)
            continue; // Kernel pages !
        else if (region.addr_mmap_start == (uintptr_t) -1)
        {
            region.addr_mmap_start = i;
            memset((void*) P2V(region.addr_mmap_start), PMM_MEM_NOTAVAILABLE, mmap_num_pages * PHYS_PAGE_SIZE);
            i += mmap_num_pages * PHYS_PAGE_SIZE;
            if (!PMM_state.kmem_initialized)
            {
                uintptr_t heap_available = (region.addr_end > i) ? (region.addr_end - i) : 0;
                size_t heap_size = (size_t) (heap_available & ~(uintptr_t) (PHYS_PAGE_SIZE - 1));
                if (heap_size > KMEM_HEAP_SIZE)
                    heap_size = KMEM_HEAP_SIZE;

                if (heap_size > (sizeof (malloc_header_t) + sizeof (uintptr_t)))
                {
                    kmem_init(P2V(i), heap_size);
                    kmem_reserved_start = i;
                    kmem_reserved_end = kmem_reserved_start + heap_size;
                    PMM_state.kmem_initialized = TRUE;
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

    if (!PMM_state.kmem_initialized)
        panic("PMM: failed to initialize kernel heap");

    if (PMM_state.num_regions >= PMM_MAX_REGIONS)
        panic("PMM: too many memory regions");

    memcpy(&PMM_state.regions[PMM_state.num_regions++], &region, sizeof (region)); // Store the current region.
}

void PMM_mmap_unset(PMM_region_t* region, int bit)
{
    uint64_t* bitmap = (uint64_t*) P2V(region->addr_mmap_start);
    bitmap[bit / 64] &= ~(1ULL << (bit % 64));
}

void PMM_mmap_set(PMM_region_t* region, int bit)
{
    uint64_t* bitmap = (uint64_t*) P2V(region->addr_mmap_start);
    bitmap[bit / 64] |= (1ULL << (bit % 64));
}

uint32_t PMM_get_index_of_free_page(PMM_region_t region)
{   
    uint32_t num_pages = (uint32_t) (region.len / PHYS_PAGE_SIZE);
    uint32_t num_words = (num_pages + 63) / 64;
    uint64_t* bitmap = (uint64_t*) P2V(region.addr_mmap_start);

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
    for (int i = 0; i < PMM_state.num_regions; i++)
    {
        PMM_region_t* region = &PMM_state.regions[i];
        while (TRUE)
        {
            uint32_t index = PMM_get_index_of_free_page(*region);
            if (index == (uint32_t) -1)
                break;

            uintptr_t page = region->addr_start + ((uintptr_t) index * PHYS_PAGE_SIZE);
            PMM_mmap_unset(region, (int) index);

            // Hard guard: never return low-memory or kernel image pages.
            // TODO : maybe find a clever way instead of hard guard.
            if (page < 0x100000 || (page >= PMM_state.kernel_phys_start && page < PMM_state.kernel_phys_end))
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
