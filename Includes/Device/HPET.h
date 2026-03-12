#ifndef _HPET_H
#define _HPET_H

#include <stdint.h>
#include <stdbool.h>

#define HPET_GENERAL_CAP_REG             0x000
#define HPET_GENERAL_CONFIG_REG          0x010
#define HPET_MAIN_COUNTER_REG            0x0F0

#define HPET_CFG_ENABLE_CNF              (1ULL << 0)
#define HPET_COUNTER_SIZE_CAP            (1ULL << 13)
#define HPET_COUNTER_CLK_PERIOD_SHIFT    32
#define HPET_COUNTER_CLK_PERIOD_MASK     0xFFFFFFFFULL

#define HPET_ADDR_SPACE_SYSTEM_MEMORY    0
#define HPET_MAX_PERIOD_FS               100000000ULL
#define HPET_WAIT_SPIN_GUARD             1000000000ULL

typedef struct HPET_runtime_state
{
    volatile uint8_t* regs;
    uint64_t period_fs;
    uint64_t frequency_hz;
    bool counter_64bit;
    bool ready;
} HPET_runtime_state_t;

bool HPET_init(void);
bool HPET_is_available(void);
bool HPET_wait_ms(uint32_t ms, uint64_t* elapsed_ticks);
bool HPET_sleep_ms(uint32_t ms);
uint64_t HPET_get_counter(void);
uint64_t HPET_get_frequency_hz(void);

#endif
