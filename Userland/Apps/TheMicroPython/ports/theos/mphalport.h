#ifndef MICROPY_INCLUDED_THEOS_MPHALPORT_H
#define MICROPY_INCLUDED_THEOS_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>

mp_uint_t mp_hal_ticks_ms(void);
void mp_hal_set_interrupt_char(int c);
int mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);

#endif // MICROPY_INCLUDED_THEOS_MPHALPORT_H
