#ifndef _ACPI_H
#define _ACPI_H

#include <Multiboot2/multiboot2.h>

#include <stdint.h>
#include <stdbool.h>

typedef struct ACPI_RSDP_descriptor10
{
    char signature[8];
    uint8_t checksum;
    char OEMID[6];
    uint8_t revision;
    uint32_t RSDT_ptr;
} __attribute__((__packed__)) ACPI_RSDP_descriptor10_t;

typedef struct ACPI_RSDP_descriptor20
{
    ACPI_RSDP_descriptor10_t first_part;
    uint32_t length;
    uint64_t XSDT_ptr;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((__packed__)) ACPI_RSDP_descriptor20_t;

typedef struct ACPI_SDT_header
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char OEMID[6];
    char OEM_table_ID[8];
    uint32_t OEM_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} ACPI_SDT_header_t;

bool ACPI_RSDP_check(struct multiboot_tag_new_acpi* rsdt_desc_ptr);
bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header_ptr);

#endif