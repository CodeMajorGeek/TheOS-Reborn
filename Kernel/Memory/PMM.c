#include <Memory/PMM.h>

static PMM_region_t** PMM_regions = (PMM_region_t**) -1;
static int PMM_num_regions = 0;

static uint64_t PMM_kernel_start;
static uint64_t PMM_kernel_end;

void PMM_init(uint64_t kernel_start, uint64_t kernel_end)
{
    PMM_kernel_start = kernel_start;
    PMM_kernel_end = kernel_end;
}

void PMM_init_region(uint64_t addr, uint64_t len)
{
    if (addr <= PMM_kernel_end && addr >= PMM_kernel_start)
    {
        printf("Kernel region detected !\n");
        return; // Kernel region !
    }

    PMM_region_t* region = (PMM_region_t*) addr;
    region->addr_start = addr + sizeof (PMM_region_t);
    region->addr_end = addr + len;
    region->len = len - sizeof (PMM_region_t);

    size_t num_pages = region->len / PMM_PHYS_PAGE_SIZE;
    size_t mmap_size = num_pages < (sizeof (uint64_t) * 8) ? 1 : num_pages / (sizeof (uint64_t) * 8) + (num_pages % (sizeof (uint64_t) * 8) == 0 ? 0 : 1);
    uint64_t mmap_num_pages = mmap_size < PMM_PHYS_PAGE_SIZE ? 1 :
        (mmap_size % PMM_PHYS_PAGE_SIZE == 0 ? mmap_size : (mmap_size / PMM_PHYS_PAGE_SIZE) + 1);

    printf("Num of allocatable pages: %d\n", num_pages);
    printf("Num of pages for mmap: %d\n", mmap_num_pages);

    memsetq((void*) region->addr_end, LONG_LONG_MAX, mmap_size); // Set all the pages to avaliable.
    
    for (uint64_t i = 0; i < mmap_num_pages; i++)
        PMM_mmap_unset(region, i);

    if (PMM_regions == (PMM_region_t**) -1) // (TEMP?) If it's the first region, let's store it temporaly and allocate a page to store region infos.
    {   
        PMM_mmap_set(region, mmap_num_pages);
        PMM_regions = (void*) (region->addr_start + (mmap_num_pages * PMM_PHYS_PAGE_SIZE));
    }
    
    PMM_regions[PMM_num_regions++] = region;
}

void PMM_mmap_unset(PMM_region_t* region, int bit)
{
    ((uint64_t*) region->addr_start)[bit / 64] &= ~(1ULL << (bit % 64));
}

void PMM_mmap_set(PMM_region_t* region, int bit)
{
    ((uint64_t*) region->addr_start)[bit / 64] |= (1ULL << (bit % 64));
}

PMM_region_alloc_t PMM_get_index_of_free_page(void)
{
    PMM_region_alloc_t region_alloc;
    region_alloc.region = (PMM_region_t*) -1;

    for (int region_index = 0; region_index < PMM_num_regions; region_index++)
    {
        PMM_region_t* region = PMM_regions[region_index];

        for (uint64_t i = 0; i < region->len / PMM_PHYS_PAGE_SIZE; i++)
        {
            for (uint8_t k = 0; k < sizeof (uint64_t) * 8; k++)
            {
                uint64_t bit = 1ULL << k;
                if (((uint64_t*) region->addr_start)[i] & bit)
                {
                    region_alloc.region = region;
                    region_alloc.index = i * sizeof (uint64_t) * 8 + k;

                    return region_alloc;
                }
            }
        }
    }

    return region_alloc;
}

void* PMM_alloc_page(void)
{
    PMM_region_alloc_t region_alloc = PMM_get_index_of_free_page();
    if (region_alloc.region == (PMM_region_t*) -1)
        return NULL;    // Out of memory.

    PMM_mmap_set(region_alloc.region, region_alloc.index);
    printf("region addr_start: 0x%H, index: %d allocated !\n", region_alloc.region->addr_start, region_alloc.index);

    return (void*) (region_alloc.region->addr_start + (region_alloc.index * PMM_PHYS_PAGE_SIZE));
}