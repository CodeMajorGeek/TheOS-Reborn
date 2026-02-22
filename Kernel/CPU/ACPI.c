#include <CPU/ACPI.h>

#include <Memory/VMM.h>

#include <stddef.h>
#include <string.h>

static uintptr_t ACPI_RSDT_phys = 0;
static uintptr_t ACPI_XSDT_phys = 0;

static ACPI_SDT_header_t* ACPI_map_sdt(uintptr_t phys)
{
    if (phys == 0)
        return NULL;

    uintptr_t header_page_phys = phys & FRAME;
    uintptr_t header_page_virt = P2V(header_page_phys);
    VMM_map_page(header_page_virt, header_page_phys);

    ACPI_SDT_header_t* header = (ACPI_SDT_header_t*) P2V(phys);
    if (header->length < sizeof(ACPI_SDT_header_t))
        return NULL;

    VMM_map_pages(P2V(phys), phys, header->length);
    return (ACPI_SDT_header_t*) P2V(phys);
}

bool ACPI_RSDP_old_check(multiboot_uint8_t* rsdp)
{
    ACPI_RSDP_descriptor10_t* rsdp_desc10 = (ACPI_RSDP_descriptor10_t*) rsdp;

    size_t sum10 = 0;
    for (int i = 0; i < sizeof(ACPI_RSDP_descriptor10_t); i++)
        sum10 += ((uint8_t*) rsdp_desc10)[i];

    return ((uint8_t) sum10) == 0;
}

bool ACPI_RSDP_new_check(multiboot_uint8_t* rsdp)
{
    ACPI_RSDP_descriptor20_t* rsdp_desc20 = (ACPI_RSDP_descriptor20_t*) rsdp;
    uint32_t len = rsdp_desc20->length;
    if (len < sizeof(ACPI_RSDP_descriptor20_t))
        len = sizeof(ACPI_RSDP_descriptor20_t);

    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += ((uint8_t*) rsdp_desc20)[i];

    return ((uint8_t) sum) == 0;
}

bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header_ptr)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < sdt_header_ptr->length; i++)
        sum += ((uint8_t*) sdt_header_ptr)[i];

    return sum == 0;
}

void ACPI_init_RSDT(ACPI_RSDP_descriptor10_t* desc)
{
    ACPI_RSDT_phys = (uintptr_t) desc->RSDT_ptr;
}

void ACPI_init_XSDT(ACPI_RSDP_descriptor20_t* desc)
{
    ACPI_XSDT_phys = (uintptr_t) desc->XSDT_ptr;
    ACPI_RSDT_phys = (uintptr_t) desc->first_part.RSDT_ptr;
}

void* ACPI_get_table_old(ACPI_RSDT_t* rsdt, char signature[4])
{
    if (!rsdt || rsdt->header.length < sizeof(rsdt->header))
        return NULL;

    size_t entries = (rsdt->header.length - sizeof(rsdt->header)) / sizeof(uint32_t);
    for (size_t i = 0; i < entries; i++)
    {
        uintptr_t table_phys = (uintptr_t) rsdt->ptr_next_SDT[i];
        ACPI_SDT_header_t* table = ACPI_map_sdt(table_phys);
        if (!table || !ACPI_SDT_check(table))
            continue;

        if (!strncmp(table->signature, signature, 4))
            return (void*) table;
    }

    return NULL;
}

void* ACPI_get_table_new(ACPI_XSDT_t* xsdt, char signature[4])
{
    if (!xsdt || xsdt->header.length < sizeof(xsdt->header))
        return NULL;

    size_t entries = (xsdt->header.length - sizeof(xsdt->header)) / sizeof(uint64_t);
    for (size_t i = 0; i < entries; i++)
    {
        uintptr_t table_phys = (uintptr_t) xsdt->ptr_next_SDT[i];
        ACPI_SDT_header_t* table = ACPI_map_sdt(table_phys);
        if (!table || !ACPI_SDT_check(table))
            continue;

        if (!strncmp(table->signature, signature, 4))
            return (void*) table;
    }

    return NULL;
}

void* ACPI_get_table(char signature[4])
{
    if (ACPI_XSDT_phys != 0)
    {
        ACPI_XSDT_t* xsdt = (ACPI_XSDT_t*) ACPI_map_sdt(ACPI_XSDT_phys);
        if (xsdt && ACPI_SDT_check(&xsdt->header))
            return ACPI_get_table_new(xsdt, signature);
    }

    if (ACPI_RSDT_phys != 0)
    {
        ACPI_RSDT_t* rsdt = (ACPI_RSDT_t*) ACPI_map_sdt(ACPI_RSDT_phys);
        if (rsdt && ACPI_SDT_check(&rsdt->header))
            return ACPI_get_table_old(rsdt, signature);
    }

    return NULL;
}
