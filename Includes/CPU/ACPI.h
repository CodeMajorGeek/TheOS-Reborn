#ifndef _ACPI_H
#define _ACPI_H

#include <stdbool.h>
#include <stdint.h>

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
#define ACPI_HPET_SIGNATURE "HPET"
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

#define ACPI_GAS_SYSTEM_MEMORY 0
#define ACPI_GAS_SYSTEM_IO     1

#define ACPI_FADT_FLAG_HW_REDUCED_ACPI (1U << 20)

typedef enum ACPI_sleep_state
{
    ACPI_SLEEP_S0 = 0,
    ACPI_SLEEP_S1 = 1,
    ACPI_SLEEP_S2 = 2,
    ACPI_SLEEP_S3 = 3,
    ACPI_SLEEP_S4 = 4,
    ACPI_SLEEP_S5 = 5
} ACPI_sleep_state_t;

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
} __attribute__((__packed__)) ACPI_SDT_header_t;

typedef struct ACPI_RSDT {
  ACPI_SDT_header_t header;
  uint32_t ptr_next_SDT[];
} __attribute__((__packed__)) ACPI_RSDT_t;

typedef struct ACPI_XSDT {
  ACPI_SDT_header_t header;
  uint64_t ptr_next_SDT[];
} __attribute__((__packed__)) ACPI_XSDT_t;

typedef struct ACPI_generic_address
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((__packed__)) ACPI_generic_address_t;

typedef struct ACPI_HPET
{
    ACPI_SDT_header_t header;
    uint8_t hardware_rev_id;
    uint8_t comparator_info;
    uint16_t pci_vendor_id;
    ACPI_generic_address_t address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((__packed__)) ACPI_HPET_t;

typedef struct ACPI_FADT
{
    ACPI_SDT_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    ACPI_generic_address_t reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    ACPI_generic_address_t x_pm1a_evt_blk;
    ACPI_generic_address_t x_pm1b_evt_blk;
    ACPI_generic_address_t x_pm1a_cnt_blk;
    ACPI_generic_address_t x_pm1b_cnt_blk;
    ACPI_generic_address_t x_pm2_cnt_blk;
    ACPI_generic_address_t x_pm_tmr_blk;
    ACPI_generic_address_t x_gpe0_blk;
    ACPI_generic_address_t x_gpe1_blk;
    ACPI_generic_address_t sleep_control_reg;
    ACPI_generic_address_t sleep_status_reg;
} __attribute__((__packed__)) ACPI_FADT_t;

bool ACPI_RSDP_old_check(uint8_t* rsdt);
bool ACPI_RSDP_new_check(uint8_t* rsdt);
bool ACPI_SDT_check(ACPI_SDT_header_t* sdt_header);

void ACPI_init_RSDT(ACPI_RSDP_descriptor10_t* desc);
void ACPI_init_XSDT(ACPI_RSDP_descriptor20_t* desc);

void* ACPI_get_table_old(ACPI_RSDT_t* rsdt, char signature[4]);
void* ACPI_get_table_new(ACPI_XSDT_t* xsdt, char signature[4]);
void* ACPI_get_table(char signature[4]);

bool ACPI_power_init(void);
bool ACPI_is_power_ready(void);
bool ACPI_sleep(ACPI_sleep_state_t state);
bool ACPI_shutdown(void);
bool ACPI_reboot(void);

#endif
