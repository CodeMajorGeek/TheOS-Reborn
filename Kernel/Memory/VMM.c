#include <Memory/VMM.h>

#include <Storage/AHCI.h>
#include <Memory/PMM.h>
#include <Device/VGA.h>
#include <Device/TTY.h>
#include <CPU/ACPI.h>
#include <CPU/SMP.h>
#include <string.h>
#include <stdint.h>

static PML4_t* VMM_PML4;

static uintptr_t VMM_VGA_virt;
static uintptr_t VMM_AHCI_virt;

static bool VMM_recursive_active = FALSE;

static uintptr_t canonical_address(uintptr_t addr)
{
    return (uintptr_t)((int64_t)(addr << 16) >> 16);
}

static uintptr_t VMM_recursive_base(void)
{
    uintptr_t addr = ((uintptr_t)VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 21) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 12);
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
    uintptr_t addr = ((uintptr_t)VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 21) |
                     ((uintptr_t)pml4_index << 12);
    return (PDPT_t*) canonical_address(addr);
}

static PDT_t* VMM_get_pdt(uint16_t pml4_index, uint16_t pdpt_index)
{
    uintptr_t addr = ((uintptr_t)VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t)VMM_RECURSIVE_INDEX << 30) |
                     ((uintptr_t)pml4_index << 21) |
                     ((uintptr_t)pdpt_index << 12);
    return (PDT_t*) canonical_address(addr);
}

static PT_t* VMM_get_pt(uint16_t pml4_index, uint16_t pdpt_index, uint16_t pdt_index)
{
    uintptr_t addr = ((uintptr_t)VMM_RECURSIVE_INDEX << 39) |
                     ((uintptr_t)pml4_index << 30) |
                     ((uintptr_t)pdpt_index << 21) |
                     ((uintptr_t)pdt_index << 12);
    return (PT_t*) canonical_address(addr);
}

static void* VMM_alloc_table(void)
{
    void* page = PMM_alloc_page();
    if (page)
        memset(page, 0, PHYS_PAGE_SIZE);
    return page;
}

static inline void VMM_invlpg(uintptr_t virt)
{
    uintptr_t page = virt & ~(uintptr_t) 0xFFFULL;
    __asm__ __volatile__("invlpg (%0)" : : "r"(page) : "memory");
}

// TODO: Implement a fixed version of kernel mapping & recursive mapping.

static void add_attribute(uintptr_t* entry, uintptr_t attribute)
{
    *entry |= attribute;
}

static int is_present(uintptr_t* entry)
{
    return (*entry & PRESENT);
}

static uintptr_t get_address(uintptr_t* entry)
{
    return (*entry & FRAME);
}

uintptr_t VMM_get_AHCI_virt(void)
{
    return VMM_AHCI_virt;
}

void VMM_map_kernel(void)
{
    uintptr_t phys_base = PMM_get_kernel_start();
    uintptr_t phys_end = PMM_get_kernel_end();

    VMM_PML4 = (PML4_t*) VMM_alloc_table();
    VMM_identity_map_all();

    uintptr_t recursive_entry = (uintptr_t) VMM_PML4;
    add_attribute(&recursive_entry, PRESENT);
    add_attribute(&recursive_entry, WRITABLE);
    VMM_PML4->entries[VMM_RECURSIVE_INDEX] = recursive_entry;

    uintptr_t virt_addr = phys_base;

    while (phys_base < phys_end)
    {
        VMM_map_page(virt_addr, phys_base);
        phys_base += PHYS_PAGE_SIZE;
        virt_addr += PHYS_PAGE_SIZE;
    }
    
    extern void* userland_stack_top;
    extern void* userland_stack_bottom;
    
    uintptr_t stack_bottom = (uintptr_t) &userland_stack_bottom;
    uintptr_t stack_top = (uintptr_t) &userland_stack_top;
    
    // 4K alignment
    stack_bottom = stack_bottom & ~0xFFF;
    stack_top = (stack_top + 0xFFF) & ~0xFFF;
    
    for (uintptr_t addr = stack_bottom; addr < stack_top; addr += 0x1000)
    {
        VMM_map_page(addr, addr);
    }

    (void) virt_addr;
    VMM_VGA_virt = VMM_VGA_VIRT_BASE;
    VMM_AHCI_virt = VMM_AHCI_VIRT_BASE;
}

void VMM_identity_map_all(void)
{
    PMM_region_t* regions = PMM_get_regions();
    for (int i = 0; i < PMM_get_num_regions(); i++)
    {
        PMM_region_t region = regions[i];
        for (uintptr_t phys = region.addr_start; phys < region.addr_end; phys += PHYS_PAGE_SIZE)   
            VMM_map_page(phys, phys);
    }
}

void VMM_hardware_mapping(void)
{
    VMM_map_pages(VMM_VGA_virt, VGA_BUFFER_ADDRESS, VGA_BUFFER_LENGTH);    // Map the VGA textmode buffer to another location.
    TTY_set_buffer((uint16_t*) VMM_VGA_virt);                                               // Set the TTY buffer to the new virtual address.
}

void VMM_map_page_flags(uintptr_t virt, uintptr_t phys, uintptr_t flags)
{
    PDPT_t* PDPT;
    PDT_t* PDT;
    PT_t* PT;
    uintptr_t cache_flags = flags & (WRITE_THROUGH | CACHE_DISABLE);
    uintptr_t table_flags = PRESENT | WRITABLE | USER_MODE;
    uintptr_t page_flags = PRESENT | WRITABLE | USER_MODE | cache_flags;

    uint16_t pml4_index = PML4_INDEX(virt);
    uint16_t pdpt_index = PDPT_INDEX(virt);
    uint16_t pdt_index = PDT_INDEX(virt);

    PML4_t* PML4 = VMM_get_pml4();

    uintptr_t PML4_entry = PML4->entries[pml4_index];
    if (is_present(&PML4_entry))
    {
        PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) get_address(&PML4_entry);
    }
    else
    {
        PDPT = (PDPT_t*) VMM_alloc_table();
        uintptr_t PDPT_entry = (uintptr_t) PDPT;
        
        add_attribute(&PDPT_entry, table_flags);
        PML4->entries[pml4_index] = PDPT_entry;
    }

    uintptr_t PDPT_entry = PDPT->entries[pdpt_index];
    if (is_present(&PDPT_entry))
    {
        PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) get_address(&PDPT_entry);
    }
    else
    {
        PDT = (PDT_t*) VMM_alloc_table();
        uintptr_t PDT_entry = (uintptr_t) PDT;

        add_attribute(&PDT_entry, table_flags);
        PDPT->entries[pdpt_index] = PDT_entry;
    }

    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (is_present(&pdt_entry))
    {
        PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) get_address(&pdt_entry);
    }
    else
    {
        PT = (struct PT *) VMM_alloc_table();
        uintptr_t PT_entry = (uintptr_t) PT;

        add_attribute(&PT_entry, table_flags);
        PDT->entries[pdt_index] = PT_entry;
    }

    uintptr_t entry = phys & FRAME;
    add_attribute(&entry, page_flags);
    PT->entries[PT_INDEX(virt)] = entry;

    VMM_invlpg(virt);
    (void) SMP_tlb_shootdown_page(virt);
}

void VMM_map_page(uintptr_t virt, uintptr_t phys)
{
    VMM_map_page_flags(virt, phys, 0);
}

void VMM_map_pages_flags(uintptr_t virt, uintptr_t phys, size_t len, uintptr_t flags)
{
    uintptr_t virtual_address = virt;
    uintptr_t physical_address = phys;

    while (physical_address < phys + len)
    {
        VMM_map_page_flags(virtual_address, physical_address, flags);
        
        physical_address += PHYS_PAGE_SIZE;
        virtual_address += PHYS_PAGE_SIZE;
    }
}

void VMM_map_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    VMM_map_pages_flags(virt, phys, len, 0);
}

void VMM_map_mmio_uc_page(uintptr_t virt, uintptr_t phys)
{
    VMM_map_page_flags(virt, phys, WRITE_THROUGH | CACHE_DISABLE);
}

void VMM_map_mmio_uc_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    VMM_map_pages_flags(virt, phys, len, WRITE_THROUGH | CACHE_DISABLE);
}

void VMM_load_cr3(void)
{
    uintptr_t pml4_addr = (uintptr_t) VMM_PML4;
    __asm__ __volatile__("mov %0, %%cr3":: "b"(pml4_addr));
    // Keep recursive mode disabled for now; the kernel relies on identity-mapped
    // physical addresses for page-table access.
    VMM_recursive_active = FALSE;
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

    PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) get_address(&pml4_entry);
    uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
    if (!is_present(&pdpt_entry))
        return FALSE;

    PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) get_address(&pdpt_entry);
    uintptr_t pdt_entry = PDT->entries[pdt_index];
    if (!is_present(&pdt_entry))
        return FALSE;

    PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) get_address(&pdt_entry);
    uintptr_t pt_entry = PT->entries[pt_index];
    if (!is_present(&pt_entry))
        return FALSE;

    *phys_out = get_address(&pt_entry) | (virt & 0xFFF);
    return TRUE;
}

bool VMM_phys_to_virt(uintptr_t phys, uintptr_t* virt_out)
{
    uintptr_t phys_frame = phys & FRAME;
    uintptr_t offset = phys & 0xFFF;

    PML4_t* PML4 = VMM_get_pml4();
    for (uint16_t pml4_index = 0; pml4_index < 512; ++pml4_index)
    {
        uintptr_t pml4_entry = PML4->entries[pml4_index];
        if (!is_present(&pml4_entry))
            continue;

        PDPT_t* PDPT = VMM_recursive_active ? VMM_get_pdpt(pml4_index) : (PDPT_t*) get_address(&pml4_entry);
        for (uint16_t pdpt_index = 0; pdpt_index < 512; ++pdpt_index)
        {
            uintptr_t pdpt_entry = PDPT->entries[pdpt_index];
            if (!is_present(&pdpt_entry))
                continue;

            PDT_t* PDT = VMM_recursive_active ? VMM_get_pdt(pml4_index, pdpt_index) : (PDT_t*) get_address(&pdpt_entry);
            for (uint16_t pdt_index = 0; pdt_index < 512; ++pdt_index)
            {
                uintptr_t pdt_entry = PDT->entries[pdt_index];
                if (!is_present(&pdt_entry))
                    continue;

                PT_t* PT = VMM_recursive_active ? VMM_get_pt(pml4_index, pdpt_index, pdt_index) : (PT_t*) get_address(&pdt_entry);
                for (uint16_t pt_index = 0; pt_index < 512; ++pt_index)
                {
                    uintptr_t pt_entry = PT->entries[pt_index];
                    if (!is_present(&pt_entry))
                        continue;

                    if ((pt_entry & FRAME) == phys_frame)
                    {
                        uintptr_t virt = ((uintptr_t)pml4_index << 39) |
                                         ((uintptr_t)pdpt_index << 30) |
                                         ((uintptr_t)pdt_index << 21) |
                                         ((uintptr_t)pt_index << 12) |
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
