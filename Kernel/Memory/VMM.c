#include <Memory/VMM.h>

#include <Memory/PMM.h>
#include <Device/VGA.h>
#include <Device/TTY.h>
#include <CPU/SMP.h>
#include <Debug/KDebug.h>

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

_Static_assert(((VMM_HHDM_BASE >> 39) & 0x1FFULL) == VMM_HHDM_PML4_INDEX,
               "VMM layout mismatch: HHDM base and PML4 index disagree");
_Static_assert(VMM_HHDM_BASE == VMM_KERNEL_SPACE_MIN,
               "VMM layout mismatch: kernel space min must match HHDM base");
_Static_assert(VMM_HHDM_BASE < VMM_MMIO_BASE,
               "VMM layout mismatch: HHDM must be below MMIO window");
_Static_assert(VMM_MMIO_BASE < VMM_KERNEL_VIRT_BASE,
               "VMM layout mismatch: MMIO window must stay below kernel text mapping");

static PML4_t* VMM_PML4 = NULL;
static uintptr_t VMM_PML4_phys = 0;

static uintptr_t VMM_VGA_virt = 0;
static uintptr_t VMM_AHCI_virt = 0;

static bool VMM_recursive_active = FALSE;
static bool VMM_cr3_loaded = FALSE;
static bool VMM_startup_identity_map_active = false;

#define VMM_STARTUP_IDENTITY_LOW_LIMIT 0x100000ULL

static uintptr_t canonical_address(uintptr_t addr)
{
    return (uintptr_t) ((int64_t) (addr << 16) >> 16);
}

static bool VMM_is_mmio_window_addr(uintptr_t virt)
{
    return virt >= VMM_MMIO_BASE && virt < VMM_KERNEL_VIRT_BASE;
}

static bool VMM_is_mmio_window_range(uintptr_t virt, size_t len)
{
    if (len == 0)
        return true;

    uintptr_t end = virt + (uintptr_t) len - 1U;
    if (end < virt)
        return false;

    return VMM_is_mmio_window_addr(virt) && VMM_is_mmio_window_addr(end);
}

static uintptr_t VMM_recursive_base(void)
{
    uintptr_t addr = ((uintptr_t) VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 21) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 12);
    return canonical_address(addr);
}

static PML4_t* VMM_get_pml4(void)
{
    if (VMM_recursive_active)
        return (PML4_t*) VMM_recursive_base();
    return VMM_PML4;
}

static PDPT_t* VMM_get_pdpt(uint16_t pml4_index)
{
    uintptr_t addr = ((uintptr_t) VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 21) |
                     ((uintptr_t) pml4_index << 12);
    return (PDPT_t*) canonical_address(addr);
}

static PDT_t* VMM_get_pdt(uint16_t pml4_index, uint16_t pdpt_index)
{
    uintptr_t addr = ((uintptr_t) VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t) VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t) pml4_index << 21) |
                     ((uintptr_t) pdpt_index << 12);
    return (PDT_t*) canonical_address(addr);
}

static PT_t* VMM_get_pt(uint16_t pml4_index, uint16_t pdpt_index, uint16_t pdt_index)
{
    uintptr_t addr = ((uintptr_t) VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t) pml4_index << 30) |
                     ((uintptr_t) pdpt_index << 21) |
                     ((uintptr_t) pdt_index << 12);
    return (PT_t*) canonical_address(addr);
}

static uintptr_t VMM_hhdm_to_phys(uintptr_t virt)
{
    return virt - VMM_HHDM_BASE;
}

static uintptr_t VMM_phys_to_hhdm(uintptr_t phys)
{
    return VMM_HHDM_BASE + phys;
}

static void* VMM_alloc_table(void)
{
    uintptr_t page_phys = (uintptr_t) PMM_alloc_page();
    if (page_phys == 0)
        return NULL;

    void* page_virt = (void*) VMM_phys_to_hhdm(page_phys);
    memset(page_virt, 0, PHYS_PAGE_SIZE);
    return page_virt;
}

static inline void VMM_invlpg(uintptr_t virt)
{
    uintptr_t page = virt & ~(uintptr_t) 0xFFFULL;
    __asm__ __volatile__("invlpg (%0)" : : "r"(page) : "memory");
}

static bool VMM_interrupts_enabled(void)
{
    uint64_t rflags = 0;
    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(rflags));
    return (rflags & (1ULL << 9)) != 0;
}

static void add_attribute(uintptr_t* entry, uintptr_t attribute)
{
    *entry |= attribute;
}

static int is_present(uintptr_t* entry)
{
    return (*entry & PRESENT) != 0;
}

static uintptr_t get_address(uintptr_t* entry)
{
    return *entry & FRAME;
}

static uint64_t VMM_page_count_aligned(uintptr_t start, uintptr_t end)
{
    if (end <= start)
        return 0;

    uintptr_t page_mask = (uintptr_t) PHYS_PAGE_SIZE - 1U;
    uintptr_t first = start & ~page_mask;
    uintptr_t limit = (end + page_mask) & ~page_mask;
    if (limit < end || limit < first)
        return 0;

    return (uint64_t) ((limit - first) / PHYS_PAGE_SIZE);
}

static uint64_t VMM_map_startup_identity_range(uintptr_t start, uintptr_t end)
{
    if (end <= start)
        return 0;

    uintptr_t page_mask = (uintptr_t) PHYS_PAGE_SIZE - 1U;
    uintptr_t page = start & ~page_mask;
    uintptr_t limit = (end + page_mask) & ~page_mask;
    if (limit < end)
        return 0;

    uint64_t mapped = 0;
    while (page < limit)
    {
        VMM_map_page(page, page);
        mapped++;
        page += PHYS_PAGE_SIZE;
    }

    return mapped;
}

static void VMM_map_startup_identity(void)
{
    if (VMM_startup_identity_map_active)
        return;

    uint64_t total_pages = 0;

    // Keep low memory identity-mapped during early bring-up (trampoline/firmware handoff paths).
    uint64_t low_pages = VMM_map_startup_identity_range(0, VMM_STARTUP_IDENTITY_LOW_LIMIT);
    total_pages += low_pages;
    kdebug_printf("[VMM] startup identity keep low [0x0..0x%llX) pages=%llu\n",
                  (unsigned long long) VMM_STARTUP_IDENTITY_LOW_LIMIT,
                  (unsigned long long) low_pages);

    PMM_region_t* regions = PMM_get_regions();
    for (int i = 0; i < PMM_get_num_regions(); i++)
    {
        PMM_region_t region = regions[i];
        uint64_t region_pages = VMM_map_startup_identity_range(region.addr_start, region.addr_end);
        total_pages += region_pages;
        if (region_pages != 0)
        {
            kdebug_printf("[VMM] startup identity keep region[%d] [0x%llX..0x%llX) pages=%llu\n",
                          i,
                          (unsigned long long) region.addr_start,
                          (unsigned long long) region.addr_end,
                          (unsigned long long) region_pages);
        }
    }

    VMM_startup_identity_map_active = true;
    kdebug_printf("[VMM] startup identity active total_pages=%llu\n",
                  (unsigned long long) total_pages);
}

static bool VMM_unmap_page_local(uintptr_t virt)
{
    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);
    uint16_t pt_index = PT_INDEX(virt);

    PML4_t* PML4 = VMM_get_pml4();
    uintptr_t pml4_entry = PML4->entries[pml4_index];
    if (!is_present(&pml4_entry))
        return false;

    PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) VMM_phys_to_hhdm(get_address(&pml4_entry));
    uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
    if (!is_present(&pdpt_entry))
        return false;

    PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) VMM_phys_to_hhdm(get_address(&pdpt_entry));
    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (!is_present(&pdt_entry))
        return false;

    PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) VMM_phys_to_hhdm(get_address(&pdt_entry));
    uintptr_t entry = PT->entries[pt_index];
    if (!is_present(&entry))
        return false;

    PT->entries[pt_index] = 0;
    return true;
}

static uint64_t VMM_unmap_startup_identity_range(uintptr_t start, uintptr_t end)
{
    if (end <= start)
        return 0;

    uintptr_t page_mask = (uintptr_t) PHYS_PAGE_SIZE - 1U;
    uintptr_t page = start & ~page_mask;
    uintptr_t limit = (end + page_mask) & ~page_mask;
    if (limit < end)
        return 0;

    uint64_t unmapped = 0;
    while (page < limit)
    {
        if (VMM_unmap_page_local(page))
            unmapped++;
        page += PHYS_PAGE_SIZE;
    }

    return unmapped;
}

uintptr_t VMM_get_AHCI_virt(void)
{
    return VMM_AHCI_virt;
}

void VMM_map_kernel(void)
{
    uintptr_t kernel_phys_start = PMM_get_kernel_start();
    uintptr_t kernel_phys_end = PMM_get_kernel_end();
    uintptr_t kernel_virt_start = PMM_get_kernel_virt_start();

    VMM_PML4 = (PML4_t*) VMM_alloc_table();
    if (!VMM_PML4)
        panic("VMM: failed to allocate PML4");

    VMM_PML4_phys = VMM_hhdm_to_phys((uintptr_t) VMM_PML4);

    uintptr_t recursive_entry = VMM_PML4_phys;
    add_attribute(&recursive_entry, PRESENT);
    add_attribute(&recursive_entry, WRITABLE);
    VMM_PML4->entries[VMM_RECURSIVE_INDEX] = recursive_entry;

    uint64_t kernel_pages = 0;
    for (uintptr_t phys = kernel_phys_start; phys < kernel_phys_end; phys += PHYS_PAGE_SIZE)
    {
        uintptr_t virt = kernel_virt_start + (phys - kernel_phys_start);
        VMM_map_page(virt, phys);
        kernel_pages++;
    }
    kdebug_printf("[VMM] kernel map phys=[0x%llX..0x%llX) virt=[0x%llX..0x%llX) pages=%llu\n",
                  (unsigned long long) kernel_phys_start,
                  (unsigned long long) kernel_phys_end,
                  (unsigned long long) kernel_virt_start,
                  (unsigned long long) (kernel_virt_start + (kernel_phys_end - kernel_phys_start)),
                  (unsigned long long) kernel_pages);

    PMM_region_t* regions = PMM_get_regions();
    uint64_t hhdm_total_pages = 0;
    for (int i = 0; i < PMM_get_num_regions(); i++)
    {
        PMM_region_t region = regions[i];
        uint64_t region_pages = 0;
        for (uintptr_t phys = region.addr_start; phys < region.addr_end; phys += PHYS_PAGE_SIZE)
        {
            VMM_map_page(VMM_phys_to_hhdm(phys), phys);
            region_pages++;
        }
        hhdm_total_pages += region_pages;
        if (region_pages != 0)
        {
            kdebug_printf("[VMM] HHDM map region[%d] phys=[0x%llX..0x%llX) virt=[0x%llX..0x%llX) pages=%llu\n",
                          i,
                          (unsigned long long) region.addr_start,
                          (unsigned long long) region.addr_end,
                          (unsigned long long) VMM_phys_to_hhdm(region.addr_start),
                          (unsigned long long) VMM_phys_to_hhdm(region.addr_end),
                          (unsigned long long) region_pages);
        }
    }
    kdebug_printf("[VMM] HHDM map total pages=%llu base=0x%llX\n",
                  (unsigned long long) hhdm_total_pages,
                  (unsigned long long) VMM_HHDM_BASE);

    VMM_map_startup_identity();

    VMM_VGA_virt = VMM_VGA_VIRT_BASE;
    VMM_AHCI_virt = VMM_AHCI_VIRT_BASE;
}

void VMM_hardware_mapping(void)
{
    VMM_map_mmio_uc_pages(VMM_VGA_virt, VGA_BUFFER_ADDRESS, VGA_BUFFER_LENGTH);
    TTY_set_buffer((uint16_t*) VMM_VGA_virt);
}

void VMM_drop_startup_identity_map(void)
{
    if (!VMM_startup_identity_map_active)
    {
        kdebug_puts("[VMM] startup identity already dropped\n");
        return;
    }

    uint64_t total_unmapped = 0;
    uint64_t low_unmapped = VMM_unmap_startup_identity_range(0, VMM_STARTUP_IDENTITY_LOW_LIMIT);
    total_unmapped += low_unmapped;
    kdebug_printf("[VMM] startup identity drop low [0x0..0x%llX) pages=%llu\n",
                  (unsigned long long) VMM_STARTUP_IDENTITY_LOW_LIMIT,
                  (unsigned long long) low_unmapped);

    PMM_region_t* regions = PMM_get_regions();
    for (int i = 0; i < PMM_get_num_regions(); i++)
    {
        PMM_region_t region = regions[i];
        uint64_t region_unmapped = VMM_unmap_startup_identity_range(region.addr_start, region.addr_end);
        total_unmapped += region_unmapped;
        if (region_unmapped != 0)
        {
            kdebug_printf("[VMM] startup identity drop region[%d] [0x%llX..0x%llX) pages=%llu/%llu\n",
                          i,
                          (unsigned long long) region.addr_start,
                          (unsigned long long) region.addr_end,
                          (unsigned long long) region_unmapped,
                          (unsigned long long) VMM_page_count_aligned(region.addr_start, region.addr_end));
        }
    }

    bool tlb_flush_ok = true;
    if (VMM_cr3_loaded)
        tlb_flush_ok = SMP_tlb_shootdown_all();

    VMM_startup_identity_map_active = false;
    kdebug_printf("[VMM] startup identity dropped total_unmapped=%llu tlb_flush=%s\n",
                  (unsigned long long) total_unmapped,
                  tlb_flush_ok ? "ok" : "failed");
}

void VMM_map_page_flags(uintptr_t virt, uintptr_t phys, uintptr_t flags)
{
    PDPT_t* PDPT;
    PDT_t* PDT;
    PT_t* PT;
    bool is_user_mapping = (flags & USER_MODE) != 0;
    if (is_user_mapping && virt >= VMM_KERNEL_SPACE_MIN)
        panic("VMM: user mapping requested in kernel higher-half");

    uintptr_t cache_flags = flags & (WRITE_THROUGH | CACHE_DISABLE);
    uintptr_t table_flags = PRESENT | WRITABLE;
    uintptr_t page_flags = PRESENT | WRITABLE | cache_flags;
    if (is_user_mapping)
    {
        table_flags |= USER_MODE;
        page_flags |= USER_MODE;
    }

    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);

    PML4_t* PML4 = VMM_get_pml4();

    uintptr_t PML4_entry = PML4->entries[pml4_index];
    if (is_present(&PML4_entry))
    {
        if (is_user_mapping && (PML4_entry & USER_MODE) == 0)
        {
            PML4_entry |= USER_MODE;
            PML4->entries[pml4_index] = PML4_entry;
        }
        PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) VMM_phys_to_hhdm(get_address(&PML4_entry));
    }
    else
    {
        PDPT = (PDPT_t*) VMM_alloc_table();
        if (!PDPT)
            panic("VMM: failed to allocate PDPT");
        uintptr_t PDPT_entry = VMM_hhdm_to_phys((uintptr_t) PDPT);

        add_attribute(&PDPT_entry, table_flags);
        PML4->entries[pml4_index] = PDPT_entry;
    }

    uintptr_t PDPT_entry = PDPT->entries[pdpt_index];
    if (is_present(&PDPT_entry))
    {
        if (is_user_mapping && (PDPT_entry & USER_MODE) == 0)
        {
            PDPT_entry |= USER_MODE;
            PDPT->entries[pdpt_index] = PDPT_entry;
        }
        PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) VMM_phys_to_hhdm(get_address(&PDPT_entry));
    }
    else
    {
        PDT = (PDT_t*) VMM_alloc_table();
        if (!PDT)
            panic("VMM: failed to allocate PDT");
        uintptr_t PDT_entry = VMM_hhdm_to_phys((uintptr_t) PDT);

        add_attribute(&PDT_entry, table_flags);
        PDPT->entries[pdpt_index] = PDT_entry;
    }

    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (is_present(&pdt_entry))
    {
        if (is_user_mapping && (pdt_entry & USER_MODE) == 0)
        {
            pdt_entry |= USER_MODE;
            PDT->entries[pdt_index] = pdt_entry;
        }
        PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) VMM_phys_to_hhdm(get_address(&pdt_entry));
    }
    else
    {
        PT = (PT_t*) VMM_alloc_table();
        if (!PT)
            panic("VMM: failed to allocate PT");
        uintptr_t PT_entry = VMM_hhdm_to_phys((uintptr_t) PT);

        add_attribute(&PT_entry, table_flags);
        PDT->entries[pdt_index] = PT_entry;
    }

    uintptr_t entry = phys & FRAME;
    add_attribute(&entry, page_flags);
    PT->entries[PT_INDEX(virt)] = entry;

    if (VMM_cr3_loaded)
    {
        VMM_invlpg(virt);
        if (VMM_interrupts_enabled())
            (void) SMP_tlb_shootdown_page(virt);
    }
}

void VMM_map_page(uintptr_t virt, uintptr_t phys)
{
    VMM_map_page_flags(virt, phys, 0);
}

void VMM_map_pages_flags(uintptr_t virt, uintptr_t phys, size_t len, uintptr_t flags)
{
    if (len == 0)
        return;

    size_t num_pages = (len + PHYS_PAGE_SIZE - 1) / PHYS_PAGE_SIZE;
    for (size_t page = 0; page < num_pages; page++)
    {
        uintptr_t virtual_address = virt + (page * PHYS_PAGE_SIZE);
        uintptr_t physical_address = phys + (page * PHYS_PAGE_SIZE);
        VMM_map_page_flags(virtual_address, physical_address, flags);
    }
}

void VMM_map_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    VMM_map_pages_flags(virt, phys, len, 0);
}

void VMM_map_user_page(uintptr_t virt, uintptr_t phys)
{
    VMM_map_page_flags(virt, phys, USER_MODE);
}

void VMM_map_user_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    VMM_map_pages_flags(virt, phys, len, USER_MODE);
}

void VMM_map_mmio_uc_page(uintptr_t virt, uintptr_t phys)
{
    if (!VMM_is_mmio_window_range(virt, PHYS_PAGE_SIZE))
        panic("VMM: MMIO page mapping outside MMIO window");

    VMM_map_page_flags(virt, phys, WRITE_THROUGH | CACHE_DISABLE);
}

void VMM_map_mmio_uc_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    if (!VMM_is_mmio_window_range(virt, len))
        panic("VMM: MMIO range mapping outside MMIO window");

    VMM_map_pages_flags(virt, phys, len, WRITE_THROUGH | CACHE_DISABLE);
}

void VMM_load_cr3(void)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(VMM_PML4_phys) : "memory");
    VMM_recursive_active = FALSE;
    VMM_cr3_loaded = TRUE;
}

bool VMM_virt_to_phys(uintptr_t virt, uintptr_t* phys_out)
{
    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);
    uint16_t pt_index = PT_INDEX(virt);

    PML4_t* PML4 = VMM_get_pml4();
    uintptr_t pml4_entry = PML4->entries[pml4_index];
    if (!is_present(&pml4_entry))
        return FALSE;

    PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) VMM_phys_to_hhdm(get_address(&pml4_entry));
    uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
    if (!is_present(&pdpt_entry))
        return FALSE;

    PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) VMM_phys_to_hhdm(get_address(&pdpt_entry));
    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (!is_present(&pdt_entry))
        return FALSE;

    PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) VMM_phys_to_hhdm(get_address(&pdt_entry));
    uintptr_t pt_entry = PT->entries[pt_index];
    if (!is_present(&pt_entry))
        return FALSE;

    *phys_out = get_address(&pt_entry) | (virt & 0xFFFULL);
    return TRUE;
}

bool VMM_is_user_accessible(uintptr_t virt)
{
    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);
    uint16_t pt_index = PT_INDEX(virt);

    PML4_t* PML4 = VMM_get_pml4();
    uintptr_t pml4_entry = PML4->entries[pml4_index];
    if (!is_present(&pml4_entry) || (pml4_entry & USER_MODE) == 0)
        return FALSE;

    PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) VMM_phys_to_hhdm(get_address(&pml4_entry));
    uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
    if (!is_present(&pdpt_entry) || (pdpt_entry & USER_MODE) == 0)
        return FALSE;

    PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) VMM_phys_to_hhdm(get_address(&pdpt_entry));
    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (!is_present(&pdt_entry) || (pdt_entry & USER_MODE) == 0)
        return FALSE;

    PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) VMM_phys_to_hhdm(get_address(&pdt_entry));
    uintptr_t pt_entry = PT->entries[pt_index];
    if (!is_present(&pt_entry) || (pt_entry & USER_MODE) == 0)
        return FALSE;

    return TRUE;
}

bool VMM_phys_to_virt(uintptr_t phys, uintptr_t* virt_out)
{
    if (!virt_out)
        return FALSE;

    uintptr_t hhdm_virt = VMM_phys_to_hhdm(phys);
    uintptr_t hhdm_back = 0;
    if (VMM_virt_to_phys(hhdm_virt, &hhdm_back) && hhdm_back == phys)
    {
        *virt_out = hhdm_virt;
        return TRUE;
    }

    uintptr_t kernel_phys_start = PMM_get_kernel_start();
    uintptr_t kernel_phys_end = PMM_get_kernel_end();
    if (phys >= kernel_phys_start && phys < kernel_phys_end)
    {
        uintptr_t kernel_virt = PMM_get_kernel_virt_start() + (phys - kernel_phys_start);
        uintptr_t kernel_back = 0;
        if (VMM_virt_to_phys(kernel_virt, &kernel_back) && kernel_back == phys)
        {
            *virt_out = kernel_virt;
            return TRUE;
        }
    }

    PML4_t* PML4 = VMM_get_pml4();
    uintptr_t phys_frame = phys & FRAME;
    uintptr_t offset = phys & 0xFFFULL;

    for (uint16_t pml4_index = 0; pml4_index < 512; pml4_index++)
    {
        uintptr_t pml4_entry = PML4->entries[pml4_index];
        if (!is_present(&pml4_entry))
            continue;

        PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) VMM_phys_to_hhdm(get_address(&pml4_entry));
        for (uint16_t pdpt_index = 0; pdpt_index < 512; pdpt_index++)
        {
            uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
            if (!is_present(&pdpt_entry))
                continue;

            PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) VMM_phys_to_hhdm(get_address(&pdpt_entry));
            for (uint16_t pdt_index = 0; pdt_index < 512; pdt_index++)
            {
                uintptr_t pdt_entry = PDT->entries[pdt_index];
                if (!is_present(&pdt_entry))
                    continue;

                PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) VMM_phys_to_hhdm(get_address(&pdt_entry));
                for (uint16_t pt_index = 0; pt_index < 512; pt_index++)
                {
                    uintptr_t pt_entry = PT->entries[pt_index];
                    if (!is_present(&pt_entry))
                        continue;

                    if ((pt_entry & FRAME) == phys_frame)
                    {
                        uintptr_t virt = ((uintptr_t) pml4_index << 39) |
                                         ((uintptr_t) pdpt_index << 30) |
                                         ((uintptr_t) pdt_index << 21) |
                                         ((uintptr_t) pt_index << 12) |
                                         offset;
                        *virt_out = canonical_address(virt);
                        return TRUE;
                    }
                }
            }
        }
    }

    return FALSE;
}

bool VMM_phys_to_virt_identity(uintptr_t phys, uintptr_t* virt_out)
{
    uintptr_t phys_out = 0;
    if (!VMM_virt_to_phys(phys, &phys_out))
        return FALSE;
    if (phys_out != phys)
        return FALSE;

    *virt_out = phys;
    return TRUE;
}
