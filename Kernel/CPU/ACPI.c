#include <CPU/ACPI.h>

bool ACPI_RSDP_check(struct multiboot_tag_new_acpi* rsdp_desc_ptr)
{
    ACPI_RSDP_descriptor10_t* rsdp_desc10 = (ACPI_RSDP_descriptor10_t*) &rsdp_desc_ptr->rsdp;

    uint32_t sum10 = 0;
    for (int i = 0; i < sizeof(rsdp_desc10); ++i)
        sum10 += ((uint8_t*) rsdp_desc10 + i)[i];

    uint32_t sum20 = 0;
    for (int i = sizeof(*rsdp_desc10); i < sizeof(*rsdp_desc_ptr); ++i)
        sum20 += ((uint8_t*) rsdp_desc_ptr + i)[i];

    if (sum20 & 1)
        return false;

    return true;
}

bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header_ptr)
{
    uint8_t sum = 0;
    for (int i = 0; i < sdt_header_ptr->length; ++i)
        sum += ((uint8_t*) sdt_header_ptr)[i];

    return sum == 0;
}