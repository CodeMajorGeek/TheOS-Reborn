#ifndef _HPET_H
#define _HPET_H

#include <stdint.h>
#include <stdbool.h>

bool HPET_init(void);
bool HPET_is_available(void);
bool HPET_wait_ms(uint32_t ms, uint64_t* elapsed_ticks);
uint64_t HPET_get_counter(void);
uint64_t HPET_get_frequency_hz(void);

#endif
