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

typedef struct ACPI_RSDT {
  ACPI_SDT_header_t header;
  uint32_t* ptr_next_SDT;
} ACPI_RSDT_t;

typedef struct ACPI_XSDT {
  ACPI_SDT_header_t header;
  uint64_t* ptr_next_SDT;
} ACPI_XSDT_t;

bool ACPI_RSDP_old_check(multiboot_uint8_t* rsdt);
bool ACPI_RSDP_new_check(multiboot_uint8_t* rsdt);
bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header);

void ACPI_init_RSDT(uint32_t rsdt_ptr);
void ACPI_init_XSDT(uint64_t xsdt_ptr);

void* ACPI_get_table_old(ACPI_RSDT_t* rsdt, char signature[4]);
void* ACPI_get_table_new(ACPI_XSDT_t* xsdt, char signature[4]);
void* ACPI_get_table(char signature[4]);

#endif