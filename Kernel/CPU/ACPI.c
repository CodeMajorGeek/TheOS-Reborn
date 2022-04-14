#include <CPU/ACPI.h>

#include <stddef.h>
#include <string.h>

static ACPI_RSDT_t* ACPI_RSDT = (ACPI_RSDT_t*) NULL;
static ACPI_XSDT_t* ACPI_XSDT = (ACPI_RSDT_t*) NULL;

bool ACPI_RSDP_old_check(multiboot_uint8_t* rsdp)
{
    ACPI_RSDP_descriptor10_t* rsdp_desc10 = (ACPI_RSDP_descriptor10_t*) rsdp;

    size_t sum10 = 0;
    for (int i = 0; i < sizeof(ACPI_RSDP_descriptor10_t); ++i)
        sum10 += ((uint8_t*) rsdp_desc10)[i];

    return ((uint8_t) sum10) == 0;
}

bool ACPI_RSDP_new_check(multiboot_uint8_t* rsdp)
{
    ACPI_RSDP_descriptor10_t* rsdp_desc10 = (ACPI_RSDP_descriptor10_t*) rsdp;

    size_t sum10 = 0;
    for (int i = 0; i < sizeof(ACPI_RSDP_descriptor10_t); ++i)
        sum10 += ((uint8_t*) rsdp_desc10)[i];

    if ((uint8_t) sum10)
        return false;

    ACPI_RSDP_descriptor20_t* rsdp_desc20 = (ACPI_RSDP_descriptor20_t*) rsdp;

    uint32_t sum20 = 0;
    for (int i = sizeof(ACPI_RSDP_descriptor10_t); i < sizeof(ACPI_RSDP_descriptor20_t); ++i)
        sum20 += ((uint8_t*) rsdp_desc20)[i];

    return ((uint8_t) sum20) == 0;
}

bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header_ptr)
{
    uint8_t sum = 0;
    for (int i = 0; i < sdt_header_ptr->length; ++i)
        sum += ((uint8_t*) sdt_header_ptr)[i];

    return sum == 0;
}

void ACPI_init_RSDT(uint32_t rsdt_ptr)
{
    ACPI_RSDT = (ACPI_RSDT_t*) (void*) rsdt_ptr;
}

void ACPI_init_XSDT(uint64_t xsdt_ptr)
{
    ACPI_XSDT = (ACPI_XSDT_t*) xsdt_ptr;
}

void* ACPI_get_table_old(ACPI_RSDT_t* rsdt, char signature[4])
{
    size_t entries = (rsdt->header.length - sizeof(rsdt->header)) / 4;
    for (int i = 0; i < entries; i++)
    {
        ACPI_SDT_header_t* SDT_header = (ACPI_SDT_header_t*) (void*) rsdt->ptr_next_SDT[i];
        if (!ACPI_SDT_check(SDT_header))
            continue;

        if (!strncmp(SDT_header->signature, signature, 4))
            return (void*) SDT_header;
    }

    return (void*) NULL;
}

void* ACPI_get_table_new(ACPI_XSDT_t* xsdt, char signature[4])
{
    size_t entries = (xsdt->header.length - sizeof(xsdt->header)) / 8;
    for (int i = 0; i < entries; i++)
    {
        ACPI_SDT_header_t* SDT_header = (ACPI_SDT_header_t*) xsdt->ptr_next_SDT[i];
        if (!ACPI_SDT_check(SDT_header))
            continue;

        if (!strncmp(SDT_header->signature, signature, 4))
            return (void*) SDT_header;
    }

    return (void*) NULL;
}

void* ACPI_get_table(char signature[4])
{
    if (ACPI_XSDT != NULL)
        return ACPI_get_table_new(ACPI_XSDT, signature);

    if (ACPI_RSDT != NULL)
        return ACPI_get_table_old(ACPI_RSDT, signature);

    return (void*) NULL;
}