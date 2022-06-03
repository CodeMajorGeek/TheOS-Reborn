#include <Memory/VMM.h>

#include <Device/AHCI.h>
#include <Memory/PMM.h>
#include <Device/VGA.h>
#include <Device/TTY.h>
#include <CPU/ACPI.h>

#include <stdio.h>

static PML4_t* VMM_PML4;

static uint64_t VMM_VGA_virt;
static uint64_t VMM_AHCI_MMIO_virt;

static void add_attribute(uint64_t* entry, uint64_t attribute)
{
    *entry |= attribute;
}

static int is_present(uint64_t* entry)
{
    return (*entry & PRESENT);
}

static uint64_t get_address(uint64_t* entry)
{
    return (*entry & FRAME);
}

uint64_t VMM_get_AHCI_MMIO_virt(void)
{
    return VMM_AHCI_MMIO_virt;
}

void VMM_map_kernel(void)
{
    uint64_t phys_base = PMM_get_kernel_start();
    uint64_t phys_end = PMM_get_kernel_end();

    VMM_PML4 = (PML4_t*) PMM_alloc_page();
    VMM_identity_map_all();

    uint64_t virt_addr = phys_base;

    while (phys_base < phys_end)
    {
        VMM_map_page(virt_addr, phys_base);

        phys_base += PHYS_PAGE_SIZE;
        virt_addr += PHYS_PAGE_SIZE;
    }

    VMM_VGA_virt = virt_addr;
    virt_addr += 0x5000;

    VMM_AHCI_MMIO_virt = virt_addr;
    virt_addr += 0x1000;
}

void VMM_identity_map_all(void)
{
    PMM_region_t* regions = PMM_get_regions();
    for (int i = 0; i < PMM_get_num_regions(); i++)
    {
        PMM_region_t region = regions[i];
        for (uintptr_t phys = region.addr_start; phys < region.addr_end; phys += PHYS_PAGE_SIZE)   
            VMM_map_page(phys, phys);

        printf("The region %d is now identity mapped !\n", i);
    }
}

void VMM_identity_mapping(void)
{
    VMM_map_page(VMM_VGA_virt, VGA_BUFFER_ADDRESS); // Map the VGA textmode buffer to another location.
    TTY_set_buffer((uint16_t*) VMM_VGA_virt);       // Set the TTY buffer to the new virtual address.

    VMM_map_page(VMM_AHCI_MMIO_virt, AHCI_MMIO_BUFFER_ADDRESS);
}

bool debug;
void VMM_map_page(uint64_t virt, uint64_t phys)
{
    printf("Mapping 0x%H to 0x%H !\n", virt, phys);

    PDPT_t* PDPT;
    PDT_t* PDT;
    PT_t* PT;

    uint64_t PML4_entry = VMM_PML4->entries[PML4_INDEX(virt)];
    if (is_present(&PML4_entry))
    {
        PDPT = (PDPT_t*) get_address(&PML4_entry);
    }
    else
    {
        PDPT = (PDPT_t*) PMM_alloc_page();
        uint64_t PDPT_entry = (uint64_t) PDPT;
        
        add_attribute(&PDPT_entry, PRESENT);
        add_attribute(&PDPT_entry, WRITABLE);
        VMM_PML4->entries[PML4_INDEX((uint64_t) virt)] = PDPT_entry;
    }

    uint64_t PDPT_entry = PDPT->entries[PDPT_INDEX(virt)];
    if (is_present(&PDPT_entry))
    {
        PDT = (PDT_t*) get_address(&PDPT_entry);
    }
    else
    {
        PDT = (PDT_t*) PMM_alloc_page();
        uint64_t PDT_entry = (uint64_t) PDT;

        add_attribute(&PDT_entry, PRESENT);
        add_attribute(&PDT_entry, WRITABLE);
        PDPT->entries[PDPT_INDEX((uint64_t) virt)] = PDT_entry;
    }

    uint64_t pdt_entry = PDT->entries[PDT_INDEX(virt)];
    if (is_present(&pdt_entry))
    {
        PT = (PT_t*) get_address(&pdt_entry);
    }
    else
    {
        PT = (struct PT *) PMM_alloc_page();
        uint64_t PT_entry = (uint64_t) PT;

        add_attribute(&PT_entry, PRESENT);
        add_attribute(&PT_entry, WRITABLE);
        PDT->entries[PDT_INDEX(virt)] = PT_entry;
    }

    uint64_t entry = phys;
    add_attribute(&entry, PRESENT);
    add_attribute(&entry, WRITABLE);
    PT->entries[PT_INDEX(virt)] = entry;
}

void VMM_map_pages(uintptr_t virt, uintptr_t phys, size_t len)
{
    uintptr_t virtual_address = virt;
    uintptr_t physical_address = phys;

    while (physical_address < phys + len)
    {
        printf("Page 0x%H is now identity mapped !\n", physical_address);
        VMM_map_page(virtual_address, physical_address);
        
        physical_address += PHYS_PAGE_SIZE;
        virtual_address += PHYS_PAGE_SIZE;
    }
}

void VMM_load_cr3(void)
{
    uint64_t pml4_addr = (uint64_t) VMM_PML4;
    __asm__ __volatile__("mov %0, %%cr3":: "b"(pml4_addr));

    debug = true;
}