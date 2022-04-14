#ifndef _ACPI_H
#define _ACPI_H

#include <Multiboot2/multiboot2.h>

#include <stdint.h>
#include <stdbool.h>

#define ACPI_APIC_SIGNATURE "APIC"
#define ACPI_BERT_SIGNATURE "BERT"
#define ACPI_CPEP_SIGNATURE "CPEP"
#define ACPI_DSDT_SIGNATURE "DSDT"
#define ACPI_ECDT_SIGNATURE "ECDT"
#define ACPI_EINJ_SIGNATURE "EINJ"
#define ACPI_ERST_SIGNATURE "ERST"
#define ACPI_FACP_SIGNATURE "FACP"
#define ACPI_FACS_SIGNATURE "FACS"
#define ACPI_HEST_SIGNATURE "HEST"
#define ACPI_MSCT_SIGNATURE "MSCT"
#define ACPI_MPST_SIGNATURE "MPST"
#define ACPI_OEMx_SIGNATURE "OEM"
#define ACPI_PMTT_SIGNATURE "PMTT"
#define ACPI_PSDT_SIGNATURE "PSDT"
#define ACPI_RASF_SIGNATURE "RASF"
#define ACPI_RSDT_SIGNATURE "RSDT"
#define ACPI_SBST_SIGNATURE "SBST"
#define ACPI_SLIT_SIGNATURE "SLIT"
#define ACPI_SRAT_SIGNATURE "SRAT"
#define ACPI_SSDT_SIGNATURE "SSDT"
#define ACPI_XSDT_SIGNATURE "XSDT"

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
  uint32_t ptr_next_SDT[];
} ACPI_RSDT_t;

typedef struct ACPI_XSDT {
  ACPI_SDT_header_t header;
  uint64_t ptr_next_SDT[];
} ACPI_XSDT_t;

bool ACPI_RSDP_old_check(multiboot_uint8_t* rsdt);
bool ACPI_RSDP_new_check(multiboot_uint8_t* rsdt);
bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header);

void ACPI_init_RSDT(ACPI_RSDP_descriptor10_t* desc);
void ACPI_init_XSDT(ACPI_RSDP_descriptor20_t* desc);

void* ACPI_get_table_old(ACPI_RSDT_t* rsdt, char signature[4]);
void* ACPI_get_table_new(ACPI_XSDT_t* xsdt, char signature[4]);
void* ACPI_get_table(char signature[4]);

#endif