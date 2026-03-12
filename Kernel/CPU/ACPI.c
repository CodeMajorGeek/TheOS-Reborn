#include <CPU/ACPI.h>

#include <CPU/IO.h>
#include <CPU/x86.h>
#include <Debug/KDebug.h>
#include <Memory/VMM.h>

#include <stddef.h>
#include <string.h>

static ACPI_runtime_state_t ACPI_runtime_state;

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

    uintptr_t map_phys_start = phys & FRAME;
    uintptr_t map_phys_end = (phys + (uintptr_t) header->length + 0x1000U - 1U) & FRAME;
    uintptr_t map_len = map_phys_end - map_phys_start;
    VMM_map_pages(P2V(map_phys_start), map_phys_start, map_len);
    return (ACPI_SDT_header_t*) P2V(phys);
}

static bool ACPI_fadt_has_field(const ACPI_FADT_t* fadt, size_t offset, size_t field_size)
{
    if (!fadt)
        return false;

    uint32_t length = fadt->header.length;
    if (length < sizeof(ACPI_SDT_header_t))
        return false;

    if (offset > length)
        return false;

    return field_size <= (size_t) (length - offset);
}

static bool ACPI_gas_is_supported(const ACPI_generic_address_t* gas)
{
    if (!gas || gas->address == 0)
        return false;

    return gas->address_space_id == ACPI_GAS_SYSTEM_IO ||
           gas->address_space_id == ACPI_GAS_SYSTEM_MEMORY;
}

static bool ACPI_io_read(uint16_t port, uint8_t width_bytes, uint32_t* out_value)
{
    if (!out_value)
        return false;

    switch (width_bytes)
    {
        case 1:
            *out_value = IO_inb(port);
            return true;
        case 2:
            *out_value = IO_inw(port);
            return true;
        case 4:
            *out_value = IO_inl(port);
            return true;
        default:
            return false;
    }
}

static bool ACPI_io_write(uint16_t port, uint8_t width_bytes, uint32_t value)
{
    switch (width_bytes)
    {
        case 1:
            IO_outb(port, (uint8_t) value);
            return true;
        case 2:
            IO_outw(port, (uint16_t) value);
            return true;
        case 4:
            IO_outl(port, value);
            return true;
        default:
            return false;
    }
}

static bool ACPI_mmio_read(uint64_t phys, uint8_t width_bytes, uint32_t* out_value)
{
    if (!out_value)
        return false;

    if (width_bytes != 1 && width_bytes != 2 && width_bytes != 4)
        return false;

    uintptr_t phys_addr = (uintptr_t) phys;
    uintptr_t page_phys = phys_addr & ~(uintptr_t) 0xFFFULL;
    uintptr_t page_virt = P2V(page_phys);
    VMM_map_page_flags(page_virt, page_phys, WRITE_THROUGH | CACHE_DISABLE | NO_EXECUTE);

    uintptr_t offset = phys_addr - page_phys;
    if (offset + width_bytes > 0x1000)
    {
        uintptr_t next_phys = page_phys + 0x1000;
        uintptr_t next_virt = P2V(next_phys);
        VMM_map_page_flags(next_virt, next_phys, WRITE_THROUGH | CACHE_DISABLE | NO_EXECUTE);
    }

    volatile uint8_t* reg = (volatile uint8_t*) (page_virt + offset);
    switch (width_bytes)
    {
        case 1:
            *out_value = *reg;
            return true;
        case 2:
            *out_value = *((volatile uint16_t*) reg);
            return true;
        case 4:
            *out_value = *((volatile uint32_t*) reg);
            return true;
        default:
            return false;
    }
}

static bool ACPI_mmio_write(uint64_t phys, uint8_t width_bytes, uint32_t value)
{
    if (width_bytes != 1 && width_bytes != 2 && width_bytes != 4)
        return false;

    uintptr_t phys_addr = (uintptr_t) phys;
    uintptr_t page_phys = phys_addr & ~(uintptr_t) 0xFFFULL;
    uintptr_t page_virt = P2V(page_phys);
    VMM_map_page_flags(page_virt, page_phys, WRITE_THROUGH | CACHE_DISABLE | NO_EXECUTE);

    uintptr_t offset = phys_addr - page_phys;
    if (offset + width_bytes > 0x1000)
    {
        uintptr_t next_phys = page_phys + 0x1000;
        uintptr_t next_virt = P2V(next_phys);
        VMM_map_page_flags(next_virt, next_phys, WRITE_THROUGH | CACHE_DISABLE | NO_EXECUTE);
    }

    volatile uint8_t* reg = (volatile uint8_t*) (page_virt + offset);
    switch (width_bytes)
    {
        case 1:
            *reg = (uint8_t) value;
            return true;
        case 2:
            *((volatile uint16_t*) reg) = (uint16_t) value;
            return true;
        case 4:
            *((volatile uint32_t*) reg) = value;
            return true;
        default:
            return false;
    }
}

static bool ACPI_gas_read_u32(const ACPI_generic_address_t* gas, uint8_t width_bytes, uint32_t* out_value)
{
    if (!ACPI_gas_is_supported(gas) || !out_value)
        return false;

    if (gas->register_bit_offset != 0)
        return false;

    if (gas->register_bit_width != 0)
    {
        uint32_t gas_bytes = (uint32_t) ((gas->register_bit_width + 7U) / 8U);
        if (gas_bytes < width_bytes)
            return false;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_IO)
    {
        if (gas->address > 0xFFFFULL)
            return false;
        return ACPI_io_read((uint16_t) gas->address, width_bytes, out_value);
    }

    return ACPI_mmio_read(gas->address, width_bytes, out_value);
}

static bool ACPI_gas_write_u32(const ACPI_generic_address_t* gas, uint8_t width_bytes, uint32_t value)
{
    if (!ACPI_gas_is_supported(gas))
        return false;

    if (gas->register_bit_offset != 0)
        return false;

    if (gas->register_bit_width != 0)
    {
        uint32_t gas_bytes = (uint32_t) ((gas->register_bit_width + 7U) / 8U);
        if (gas_bytes < width_bytes)
            return false;
    }

    if (gas->address_space_id == ACPI_GAS_SYSTEM_IO)
    {
        if (gas->address > 0xFFFFULL)
            return false;
        return ACPI_io_write((uint16_t) gas->address, width_bytes, value);
    }

    return ACPI_mmio_write(gas->address, width_bytes, value);
}

static bool ACPI_aml_decode_pkg_len(const uint8_t* ptr,
                                    size_t size,
                                    size_t* out_len,
                                    size_t* out_used)
{
    if (!ptr || !out_len || !out_used || size == 0)
        return false;

    uint8_t lead = ptr[0];
    uint8_t follow = (uint8_t) (lead >> 6);
    if ((size_t) follow + 1U > size)
        return false;

    size_t length = (size_t) (lead & 0x0F);
    for (uint8_t i = 0; i < follow; i++)
    {
        uint8_t b = ptr[1U + i];
        length |= ((size_t) b) << (4U + (8U * i));
    }

    *out_len = length;
    *out_used = (size_t) follow + 1U;
    return true;
}

static bool ACPI_aml_parse_integer_u8(const uint8_t* ptr,
                                      size_t size,
                                      uint8_t* out_value,
                                      size_t* out_used)
{
    if (!ptr || !out_value || !out_used || size == 0)
        return false;

    switch (ptr[0])
    {
        case 0x00:
            *out_value = 0;
            *out_used = 1;
            return true;
        case 0x01:
            *out_value = 1;
            *out_used = 1;
            return true;
        case 0xFF:
            *out_value = 0xFF;
            *out_used = 1;
            return true;
        case 0x0A:
            if (size < 2)
                return false;
            *out_value = ptr[1];
            *out_used = 2;
            return true;
        case 0x0B:
            if (size < 3)
                return false;
            *out_value = ptr[1];
            *out_used = 3;
            return true;
        case 0x0C:
            if (size < 5)
                return false;
            *out_value = ptr[1];
            *out_used = 5;
            return true;
        case 0x0E:
            if (size < 9)
                return false;
            *out_value = ptr[1];
            *out_used = 9;
            return true;
        default:
            return false;
    }
}

static void ACPI_parse_sleep_types_from_aml(const uint8_t* aml, size_t aml_len)
{
    for (size_t i = 0; i < sizeof(ACPI_runtime_state.sleep_types) / sizeof(ACPI_runtime_state.sleep_types[0]); i++)
    {
        ACPI_runtime_state.sleep_types[i].valid = false;
        ACPI_runtime_state.sleep_types[i].typ_a = 0;
        ACPI_runtime_state.sleep_types[i].typ_b = 0;
    }

    ACPI_runtime_state.sleep_types[ACPI_SLEEP_S0].valid = true;

    if (!aml || aml_len < 8)
        return;

    for (size_t i = 0; i + 6U < aml_len; i++)
    {
        if (aml[i] != 0x08)
            continue;
        if (aml[i + 1] != '_' || aml[i + 2] != 'S' || aml[i + 4] != '_')
            continue;
        if (aml[i + 3] < '0' || aml[i + 3] > '5')
            continue;

        uint8_t state = (uint8_t) (aml[i + 3] - '0');
        size_t pos = i + 5U;
        if (pos >= aml_len)
            continue;

        if (aml[pos] != 0x12)
            continue;

        size_t pkg_len = 0;
        size_t pkg_len_used = 0;
        if (!ACPI_aml_decode_pkg_len(aml + pos + 1U, aml_len - (pos + 1U), &pkg_len, &pkg_len_used))
            continue;

        size_t pkg_start = pos + 1U + pkg_len_used;
        size_t pkg_end = pos + 1U + pkg_len;
        if (pkg_start >= aml_len || pkg_end > aml_len || pkg_start >= pkg_end)
            continue;

        uint8_t elem_count = aml[pkg_start];
        if (elem_count < 2)
            continue;

        size_t cursor = pkg_start + 1U;
        uint8_t typ_a = 0;
        uint8_t typ_b = 0;
        size_t used = 0;

        if (!ACPI_aml_parse_integer_u8(aml + cursor, pkg_end - cursor, &typ_a, &used))
            continue;
        cursor += used;

        if (!ACPI_aml_parse_integer_u8(aml + cursor, pkg_end - cursor, &typ_b, &used))
            typ_b = typ_a;

        ACPI_runtime_state.sleep_types[state].valid = true;
        ACPI_runtime_state.sleep_types[state].typ_a = (uint8_t) (typ_a & 0x7U);
        ACPI_runtime_state.sleep_types[state].typ_b = (uint8_t) (typ_b & 0x7U);
    }
}

static ACPI_generic_address_t ACPI_gas_from_io_port(uint32_t port, uint8_t width_bytes)
{
    ACPI_generic_address_t gas;
    memset(&gas, 0, sizeof(gas));

    if (port == 0)
        return gas;

    gas.address_space_id = ACPI_GAS_SYSTEM_IO;
    gas.register_bit_width = (uint8_t) (width_bytes * 8U);
    gas.register_bit_offset = 0;
    gas.access_size = (width_bytes == 1) ? 1 : ((width_bytes == 2) ? 2 : 3);
    gas.address = (uint64_t) port;
    return gas;
}

static bool ACPI_try_enable_legacy_mode(void)
{
    if (ACPI_runtime_state.pm1_cnt_len == 0 || !ACPI_gas_is_supported(&ACPI_runtime_state.pm1a_cnt))
        return false;

    uint32_t pm1a = 0;
    if (!ACPI_gas_read_u32(&ACPI_runtime_state.pm1a_cnt, ACPI_runtime_state.pm1_cnt_len, &pm1a))
        return false;

    if ((pm1a & ACPI_PM1_CNT_SCI_EN_BIT) != 0)
        return true;

    if (ACPI_runtime_state.smi_cmd_port == 0 || ACPI_runtime_state.acpi_enable_value == 0)
        return false;
    if (ACPI_runtime_state.smi_cmd_port > 0xFFFFU)
        return false;

    IO_outb((uint16_t) ACPI_runtime_state.smi_cmd_port, ACPI_runtime_state.acpi_enable_value);

    for (uint32_t i = 0; i < ACPI_ENABLE_TIMEOUT_LOOPS; i++)
    {
        if (!ACPI_gas_read_u32(&ACPI_runtime_state.pm1a_cnt, ACPI_runtime_state.pm1_cnt_len, &pm1a))
            break;
        if ((pm1a & ACPI_PM1_CNT_SCI_EN_BIT) != 0)
            return true;
        __asm__ __volatile__("pause");
    }

    return false;
}

bool ACPI_RSDP_old_check(uint8_t* rsdp)
{
    ACPI_RSDP_descriptor10_t* rsdp_desc10 = (ACPI_RSDP_descriptor10_t*) rsdp;

    size_t sum10 = 0;
    for (int i = 0; i < sizeof(ACPI_RSDP_descriptor10_t); i++)
        sum10 += ((uint8_t*) rsdp_desc10)[i];

    return ((uint8_t) sum10) == 0;
}

bool ACPI_RSDP_new_check(uint8_t* rsdp)
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
    ACPI_runtime_state.rsdt_phys = (uintptr_t) desc->RSDT_ptr;
}

void ACPI_init_XSDT(ACPI_RSDP_descriptor20_t* desc)
{
    ACPI_runtime_state.xsdt_phys = (uintptr_t) desc->XSDT_ptr;
    ACPI_runtime_state.rsdt_phys = (uintptr_t) desc->first_part.RSDT_ptr;
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
    if (ACPI_runtime_state.xsdt_phys != 0)
    {
        ACPI_XSDT_t* xsdt = (ACPI_XSDT_t*) ACPI_map_sdt(ACPI_runtime_state.xsdt_phys);
        if (xsdt && ACPI_SDT_check(&xsdt->header))
            return ACPI_get_table_new(xsdt, signature);
    }

    if (ACPI_runtime_state.rsdt_phys != 0)
    {
        ACPI_RSDT_t* rsdt = (ACPI_RSDT_t*) ACPI_map_sdt(ACPI_runtime_state.rsdt_phys);
        if (rsdt && ACPI_SDT_check(&rsdt->header))
            return ACPI_get_table_old(rsdt, signature);
    }

    return NULL;
}

bool ACPI_power_init(void)
{
    if (ACPI_runtime_state.power_initialized)
        return ACPI_runtime_state.power_ready;

    ACPI_runtime_state.power_initialized = true;
    ACPI_runtime_state.power_ready = false;

    ACPI_FADT_t* fadt = (ACPI_FADT_t*) ACPI_get_table(ACPI_FACP_SIGNATURE);
    if (!fadt)
    {
        kdebug_puts("[ACPI] FADT (FACP) not found\n");
        return false;
    }

    ACPI_runtime_state.pm1a_cnt = (ACPI_generic_address_t) { 0 };
    ACPI_runtime_state.pm1b_cnt = (ACPI_generic_address_t) { 0 };
    ACPI_runtime_state.reset_reg = (ACPI_generic_address_t) { 0 };
    ACPI_runtime_state.sleep_control_reg = (ACPI_generic_address_t) { 0 };
    ACPI_runtime_state.pm1_cnt_len = 0;
    ACPI_runtime_state.smi_cmd_port = 0;
    ACPI_runtime_state.acpi_enable_value = 0;
    ACPI_runtime_state.reset_value = 0;
    ACPI_runtime_state.hw_reduced = false;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, flags), sizeof(fadt->flags)))
        ACPI_runtime_state.hw_reduced = (fadt->flags & ACPI_FADT_FLAG_HW_REDUCED_ACPI) != 0;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, pm1_cnt_len), sizeof(fadt->pm1_cnt_len)))
        ACPI_runtime_state.pm1_cnt_len = fadt->pm1_cnt_len;

    if (ACPI_runtime_state.pm1_cnt_len == 0 || ACPI_runtime_state.pm1_cnt_len > 4)
        ACPI_runtime_state.pm1_cnt_len = 2;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, x_pm1a_cnt_blk), sizeof(fadt->x_pm1a_cnt_blk)) &&
        ACPI_gas_is_supported(&fadt->x_pm1a_cnt_blk))
    {
        ACPI_runtime_state.pm1a_cnt = fadt->x_pm1a_cnt_blk;
    }
    else if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, pm1a_cnt_blk), sizeof(fadt->pm1a_cnt_blk)) &&
             fadt->pm1a_cnt_blk != 0)
    {
        ACPI_runtime_state.pm1a_cnt = ACPI_gas_from_io_port(fadt->pm1a_cnt_blk, ACPI_runtime_state.pm1_cnt_len);
    }

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, x_pm1b_cnt_blk), sizeof(fadt->x_pm1b_cnt_blk)) &&
        ACPI_gas_is_supported(&fadt->x_pm1b_cnt_blk))
    {
        ACPI_runtime_state.pm1b_cnt = fadt->x_pm1b_cnt_blk;
    }
    else if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, pm1b_cnt_blk), sizeof(fadt->pm1b_cnt_blk)) &&
             fadt->pm1b_cnt_blk != 0)
    {
        ACPI_runtime_state.pm1b_cnt = ACPI_gas_from_io_port(fadt->pm1b_cnt_blk, ACPI_runtime_state.pm1_cnt_len);
    }

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, smi_cmd), sizeof(fadt->smi_cmd)))
        ACPI_runtime_state.smi_cmd_port = fadt->smi_cmd;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, acpi_enable), sizeof(fadt->acpi_enable)))
        ACPI_runtime_state.acpi_enable_value = fadt->acpi_enable;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, reset_reg), sizeof(fadt->reset_reg)) &&
        ACPI_gas_is_supported(&fadt->reset_reg))
        ACPI_runtime_state.reset_reg = fadt->reset_reg;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, reset_value), sizeof(fadt->reset_value)))
        ACPI_runtime_state.reset_value = fadt->reset_value;

    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, sleep_control_reg), sizeof(fadt->sleep_control_reg)) &&
        ACPI_gas_is_supported(&fadt->sleep_control_reg))
        ACPI_runtime_state.sleep_control_reg = fadt->sleep_control_reg;

    uintptr_t dsdt_phys = 0;
    if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, x_dsdt), sizeof(fadt->x_dsdt)) && fadt->x_dsdt != 0)
        dsdt_phys = (uintptr_t) fadt->x_dsdt;
    else if (ACPI_fadt_has_field(fadt, offsetof(ACPI_FADT_t, dsdt), sizeof(fadt->dsdt)))
        dsdt_phys = (uintptr_t) fadt->dsdt;

    if (dsdt_phys != 0)
    {
        ACPI_SDT_header_t* dsdt = ACPI_map_sdt(dsdt_phys);
        if (dsdt && ACPI_SDT_check(dsdt) && dsdt->length > sizeof(ACPI_SDT_header_t))
        {
            const uint8_t* aml = (const uint8_t*) dsdt + sizeof(ACPI_SDT_header_t);
            size_t aml_len = dsdt->length - sizeof(ACPI_SDT_header_t);
            ACPI_parse_sleep_types_from_aml(aml, aml_len);
        }
    }

    if (!ACPI_runtime_state.hw_reduced)
    {
        bool acpi_mode_ok = ACPI_try_enable_legacy_mode();
        if (!acpi_mode_ok)
            kdebug_puts("[ACPI] legacy PM mode enable/check failed\n");
    }

    bool has_sleep_path = false;
    if (ACPI_runtime_state.hw_reduced)
        has_sleep_path = ACPI_gas_is_supported(&ACPI_runtime_state.sleep_control_reg);
    else
        has_sleep_path = ACPI_gas_is_supported(&ACPI_runtime_state.pm1a_cnt);

    bool has_reset_path = ACPI_gas_is_supported(&ACPI_runtime_state.reset_reg);
    ACPI_runtime_state.power_ready = has_sleep_path || has_reset_path;

    kdebug_printf("[ACPI] power init: hw_reduced=%u sleep=%u reset=%u s5=%u\n",
                  ACPI_runtime_state.hw_reduced ? 1U : 0U,
                  has_sleep_path ? 1U : 0U,
                  has_reset_path ? 1U : 0U,
                  ACPI_runtime_state.sleep_types[ACPI_SLEEP_S5].valid ? 1U : 0U);

    return ACPI_runtime_state.power_ready;
}

bool ACPI_is_power_ready(void)
{
    if (!ACPI_runtime_state.power_initialized)
        return ACPI_power_init();

    return ACPI_runtime_state.power_ready;
}

bool ACPI_sleep(ACPI_sleep_state_t state)
{
    if ((uint32_t) state > (uint32_t) ACPI_SLEEP_S5)
        return false;

    if (!ACPI_runtime_state.power_initialized)
        (void) ACPI_power_init();

    if (state == ACPI_SLEEP_S0)
        return true;

    if (!ACPI_runtime_state.power_ready)
        return false;

    if (!ACPI_runtime_state.sleep_types[state].valid)
    {
        kdebug_printf("[ACPI] sleep S%u unsupported: _S%u not found\n",
                      (unsigned int) state,
                      (unsigned int) state);
        return false;
    }

    uint8_t typ_a = ACPI_runtime_state.sleep_types[state].typ_a;
    uint8_t typ_b = ACPI_runtime_state.sleep_types[state].typ_b;

    if (ACPI_runtime_state.hw_reduced)
    {
        if (!ACPI_gas_is_supported(&ACPI_runtime_state.sleep_control_reg))
            return false;

        uint32_t ctrl = ((uint32_t) (typ_a & 0x7U) << ACPI_SLEEP_CTRL_SLP_TYP_SHIFT) |
                        ACPI_SLEEP_CTRL_SLP_EN_BIT;

        cli();
        bool ok = ACPI_gas_write_u32(&ACPI_runtime_state.sleep_control_reg, 1, ctrl);
        sti();
        if (!ok)
            return false;

        for (uint32_t i = 0; i < ACPI_TRANSITION_WAIT_LOOPS; i++)
            __asm__ __volatile__("hlt");

        return false;
    }

    if (!ACPI_gas_is_supported(&ACPI_runtime_state.pm1a_cnt))
        return false;

    uint32_t pm1a = 0;
    if (!ACPI_gas_read_u32(&ACPI_runtime_state.pm1a_cnt, ACPI_runtime_state.pm1_cnt_len, &pm1a))
        return false;

    uint32_t pm1a_base = (pm1a & ~(ACPI_PM1_CNT_SLP_TYP_MASK | ACPI_PM1_CNT_SLP_EN_BIT)) |
                         ((uint32_t) (typ_a & 0x7U) << ACPI_PM1_CNT_SLP_TYP_SHIFT);

    uint32_t pm1b_base = 0;
    bool has_pm1b = ACPI_gas_is_supported(&ACPI_runtime_state.pm1b_cnt);
    if (has_pm1b)
    {
        uint32_t pm1b = 0;
        if (!ACPI_gas_read_u32(&ACPI_runtime_state.pm1b_cnt, ACPI_runtime_state.pm1_cnt_len, &pm1b))
            return false;

        pm1b_base = (pm1b & ~(ACPI_PM1_CNT_SLP_TYP_MASK | ACPI_PM1_CNT_SLP_EN_BIT)) |
                    ((uint32_t) (typ_b & 0x7U) << ACPI_PM1_CNT_SLP_TYP_SHIFT);
    }

    cli();

    if (!ACPI_gas_write_u32(&ACPI_runtime_state.pm1a_cnt, ACPI_runtime_state.pm1_cnt_len, pm1a_base))
    {
        sti();
        return false;
    }

    if (has_pm1b && !ACPI_gas_write_u32(&ACPI_runtime_state.pm1b_cnt, ACPI_runtime_state.pm1_cnt_len, pm1b_base))
    {
        sti();
        return false;
    }

    if (has_pm1b)
        (void) ACPI_gas_write_u32(&ACPI_runtime_state.pm1b_cnt, ACPI_runtime_state.pm1_cnt_len, pm1b_base | ACPI_PM1_CNT_SLP_EN_BIT);
    (void) ACPI_gas_write_u32(&ACPI_runtime_state.pm1a_cnt, ACPI_runtime_state.pm1_cnt_len, pm1a_base | ACPI_PM1_CNT_SLP_EN_BIT);

    sti();

    for (uint32_t i = 0; i < ACPI_TRANSITION_WAIT_LOOPS; i++)
        __asm__ __volatile__("hlt");

    return false;
}

bool ACPI_shutdown(void)
{
    return ACPI_sleep(ACPI_SLEEP_S5);
}

bool ACPI_reboot(void)
{
    if (!ACPI_runtime_state.power_initialized)
        (void) ACPI_power_init();

    bool reboot_triggered = false;
    if (ACPI_gas_is_supported(&ACPI_runtime_state.reset_reg))
    {
        cli();
        reboot_triggered = ACPI_gas_write_u32(&ACPI_runtime_state.reset_reg, 1, ACPI_runtime_state.reset_value);
        sti();
    }
    else
    {
        // FADT reset register is optional; use chipset hard reset as fallback.
        kdebug_puts("[ACPI] reset register unavailable, trying CF9 reset fallback\n");
        cli();
        IO_outb(ACPI_CF9_RESET_PORT, 0x02U);
        IO_wait();
        IO_outb(ACPI_CF9_RESET_PORT, 0x06U);
        sti();
        reboot_triggered = true;
    }

    if (!reboot_triggered)
        return false;

    for (uint32_t i = 0; i < ACPI_TRANSITION_WAIT_LOOPS; i++)
        __asm__ __volatile__("pause");

    return false;
}
