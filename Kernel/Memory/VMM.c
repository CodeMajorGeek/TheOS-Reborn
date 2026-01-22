#include <Memory/VMM.h>

#include <Storage/AHCI.h>
#include <Memory/PMM.h>
#include <Device/VGA.h>
#include <Device/TTY.h>
#include <CPU/ACPI.h>

static PML4_t* VMM_PML4;

static uintptr_t VMM_VGA_virt;
static uintptr_t VMM_AHCI_virt;

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

    VMM_PML4 = (PML4_t*) PMM_alloc_page();
    VMM_identity_map_all();

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

    VMM_VGA_virt = virt_addr;
    virt_addr += VGA_BUFFER_LENGTH;
    VMM_AHCI_virt = virt_addr;
    virt_addr += AHCI_MEM_LENGTH;
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
    VMM_map_pages(VMM_VGA_virt, VGA_BUFFER_ADDRESS, VGA_BUFFER_LENGTH / PHYS_PAGE_SIZE);    // Map the VGA textmode buffer to another location.
    TTY_set_buffer((uint16_t*) VMM_VGA_virt);                                               // Set the TTY buffer to the new virtual address.
}

void VMM_map_page(uintptr_t virt, uintptr_t phys)
{
    PDPT_t* PDPT;
    PDT_t* PDT;
    PT_t* PT;

    uintptr_t PML4_entry = VMM_PML4->entries[PML4_INDEX(virt)];
    if (is_present(&PML4_entry))
    {
        PDPT = (PDPT_t*) get_address(&PML4_entry);
    }
    else
    {
        PDPT = (PDPT_t*) PMM_alloc_page();
        uintptr_t PDPT_entry = (uintptr_t) PDPT;
        
        add_attribute(&PDPT_entry, PRESENT);
        add_attribute(&PDPT_entry, WRITABLE);
        add_attribute(&PDPT_entry, USER_MODE);
        VMM_PML4->entries[PML4_INDEX((uintptr_t) virt)] = PDPT_entry;
    }

    uintptr_t PDPT_entry = PDPT->entries[PDPT_INDEX(virt)];
    if (is_present(&PDPT_entry))
    {
        PDT = (PDT_t*) get_address(&PDPT_entry);
    }
    else
    {
        PDT = (PDT_t*) PMM_alloc_page();
        uintptr_t PDT_entry = (uintptr_t) PDT;

        add_attribute(&PDT_entry, PRESENT);
        add_attribute(&PDT_entry, WRITABLE);
        add_attribute(&PDT_entry, USER_MODE);
        PDPT->entries[PDPT_INDEX((uintptr_t) virt)] = PDT_entry;
    }

    uintptr_t pdt_entry = PDT->entries[PDT_INDEX(virt)];
    if (is_present(&pdt_entry))
    {
        PT = (PT_t*) get_address(&pdt_entry);
    }
    else
    {
        PT = (struct PT *) PMM_alloc_page();
        uintptr_t PT_entry = (uintptr_t) PT;

        add_attribute(&PT_entry, PRESENT);
        add_attribute(&PT_entry, WRITABLE);
        add_attribute(&PT_entry, USER_MODE);
        PDT->entries[PDT_INDEX(virt)] = PT_entry;
    }

    uintptr_t entry = phys;
    add_attribute(&entry, PRESENT);
    add_attribute(&entry, WRITABLE);
    add_attribute(&entry, USER_MODE);
    PT->entries[PT_INDEX(virt)] = entry;
}

void VMM_map_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    uintptr_t virtual_address = virt;
    uintptr_t physical_address = phys;

    while (physical_address < phys + len)
    {
        VMM_map_page(virtual_address, physical_address);
        
        physical_address += PHYS_PAGE_SIZE;
        virtual_address += PHYS_PAGE_SIZE;
    }
}

void VMM_load_cr3(void)
{
    uintptr_t pml4_addr = (uintptr_t) VMM_PML4;
    __asm__ __volatile__("mov %0, %%cr3":: "b"(pml4_addr));
}