#include <Device/HPET.h>

#include <CPU/ACPI.h>
#include <Memory/VMM.h>
#include <Debug/KDebug.h>
#include <Task/Task.h>

static volatile uint8_t* HPET_regs = (volatile uint8_t*) 0;
static uint64_t HPET_period_fs = 0;
static uint64_t HPET_frequency_hz = 0;
static bool HPET_counter_64bit = false;
static bool HPET_ready = false;

static inline uint64_t HPET_read64(uint32_t offset)
{
    return *((volatile uint64_t*) (HPET_regs + offset));
}

static inline void HPET_write64(uint32_t offset, uint64_t value)
{
    *((volatile uint64_t*) (HPET_regs + offset)) = value;
}

bool HPET_init(void)
{
    if (HPET_ready)
        return true;

    ACPI_HPET_t* table = (ACPI_HPET_t*) ACPI_get_table(ACPI_HPET_SIGNATURE);
    if (!table)
    {
        kdebug_puts("[HPET] ACPI table not found\n");
        return false;
    }

    if (table->address.address_space_id != HPET_ADDR_SPACE_SYSTEM_MEMORY || table->address.address == 0)
    {
        kdebug_printf("[HPET] unsupported GAS space=%u addr=0x%llX\n",
                      table->address.address_space_id,
                      (unsigned long long) table->address.address);
        return false;
    }

    uintptr_t hpet_base_phys = (uintptr_t) table->address.address;
    uintptr_t hpet_page_phys = hpet_base_phys & ~(uintptr_t) 0xFFFULL;
    uintptr_t hpet_page_virt = VMM_MMIO_VIRT(hpet_page_phys);
    VMM_map_mmio_uc_page(hpet_page_virt, hpet_page_phys);
    HPET_regs = (volatile uint8_t*) (hpet_page_virt + (hpet_base_phys - hpet_page_phys));

    uint64_t cap = HPET_read64(HPET_GENERAL_CAP_REG);
    uint64_t period_fs = (cap >> HPET_COUNTER_CLK_PERIOD_SHIFT) & HPET_COUNTER_CLK_PERIOD_MASK;
    if (period_fs == 0 || period_fs > HPET_MAX_PERIOD_FS)
    {
        kdebug_printf("[HPET] invalid period_fs=%llu\n", (unsigned long long) period_fs);
        return false;
    }

    HPET_counter_64bit = (cap & HPET_COUNTER_SIZE_CAP) != 0;
    HPET_period_fs = period_fs;
    HPET_frequency_hz = 1000000000000000ULL / HPET_period_fs;

    uint64_t cfg = HPET_read64(HPET_GENERAL_CONFIG_REG);
    cfg &= ~HPET_CFG_ENABLE_CNF;
    HPET_write64(HPET_GENERAL_CONFIG_REG, cfg);
    HPET_write64(HPET_MAIN_COUNTER_REG, 0);
    HPET_write64(HPET_GENERAL_CONFIG_REG, cfg | HPET_CFG_ENABLE_CNF);

    HPET_ready = true;
    kdebug_printf("[HPET] enabled base=0x%llX period_fs=%llu freq=%lluHz counter=%s\n",
                  (unsigned long long) hpet_base_phys,
                  (unsigned long long) HPET_period_fs,
                  (unsigned long long) HPET_frequency_hz,
                  HPET_counter_64bit ? "64-bit" : "32-bit");
    return true;
}

bool HPET_is_available(void)
{
    return HPET_ready;
}

uint64_t HPET_get_counter(void)
{
    if (!HPET_ready)
        return 0;

    uint64_t value = HPET_read64(HPET_MAIN_COUNTER_REG);
    if (!HPET_counter_64bit)
        value &= 0xFFFFFFFFULL;

    return value;
}

uint64_t HPET_get_frequency_hz(void)
{
    return HPET_frequency_hz;
}

bool HPET_wait_ms(uint32_t ms, uint64_t* elapsed_ticks)
{
    if (!HPET_ready)
        return false;

    if (ms == 0)
    {
        if (elapsed_ticks)
            *elapsed_ticks = 0;
        return true;
    }

    const uint64_t fs_per_ms = 1000000000000ULL;
    uint64_t whole_per_ms = fs_per_ms / HPET_period_fs;
    uint64_t rem_per_ms = fs_per_ms % HPET_period_fs;

    if (whole_per_ms != 0 && ms > (uint32_t) (~0ULL / whole_per_ms))
        return false;

    uint64_t target_ticks = (uint64_t) ms * whole_per_ms;

    if (rem_per_ms != 0)
    {
        if (ms > (uint32_t) (~0ULL / rem_per_ms))
            return false;

        uint64_t rem_total = (uint64_t) ms * rem_per_ms;
        target_ticks += rem_total / HPET_period_fs;
        if ((rem_total % HPET_period_fs) != 0)
            target_ticks++;
    }

    if (target_ticks == 0)
        target_ticks = 1;

    uint64_t start = HPET_get_counter();
    uint64_t spin_guard = 0;

    if (HPET_counter_64bit)
    {
        while ((HPET_get_counter() - start) < target_ticks)
        {
            __asm__ __volatile__("pause");
            if (++spin_guard > HPET_WAIT_SPIN_GUARD)
            {
                kdebug_puts("[HPET] wait timeout\n");
                return false;
            }
        }

        if (elapsed_ticks)
            *elapsed_ticks = HPET_get_counter() - start;
    }
    else
    {
        uint32_t start32 = (uint32_t) start;
        uint32_t target32 = (target_ticks > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t) target_ticks;

        while ((uint32_t) ((uint32_t) HPET_get_counter() - start32) < target32)
        {
            __asm__ __volatile__("pause");
            if (++spin_guard > HPET_WAIT_SPIN_GUARD)
            {
                kdebug_puts("[HPET] wait timeout\n");
                return false;
            }
        }

        if (elapsed_ticks)
            *elapsed_ticks = (uint32_t) ((uint32_t) HPET_get_counter() - start32);
    }

    return true;
}

bool HPET_sleep_ms(uint32_t ms)
{
    if (ms == 0)
        return true;

    if (task_sleep_ms(ms))
        return true;

    return HPET_wait_ms(ms, NULL);
}
