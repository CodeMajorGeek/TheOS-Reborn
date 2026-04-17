#ifndef MICROPY_INCLUDED_THEOS_MPCONFIGPORT_H
#define MICROPY_INCLUDED_THEOS_MPCONFIGPORT_H

#include <stdint.h>

// Start from the smallest baseline and selectively enable what we need.
#define MICROPY_CONFIG_ROM_LEVEL         (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)
#define MICROPY_ENABLE_COMPILER          (1)
#define MICROPY_ENABLE_GC                (1)
#define MICROPY_HELPER_REPL              (1)
#define MICROPY_MODULE_FROZEN_MPY        (0)
#define MICROPY_ENABLE_EXTERNAL_IMPORT   (0)
#define MICROPY_KBD_EXCEPTION            (1)

#define MICROPY_ALLOC_PATH_MAX           (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT   (16)

#define MICROPY_PY_SYS_MODULES           (1)
#define MICROPY_PY_SYS_EXIT              (1)
#define MICROPY_PY_SYS_PATH              (0)
#define MICROPY_PY_SYS_ARGV              (1)

#define MICROPY_FLOAT_IMPL               (MICROPY_FLOAT_IMPL_NONE)
#define MICROPY_LONGINT_IMPL             (MICROPY_LONGINT_IMPL_LONGLONG)

// Avoid dependency on alloca in this constrained libc.
#define MICROPY_NO_ALLOCA                (1)

typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME            "TheOS"
#define MICROPY_HW_MCU_NAME              "x86_64"
#define MICROPY_HEAP_SIZE                (192 * 1024)

#define MP_STATE_PORT                    MP_STATE_VM

// Poll keyboard while the VM runs so Ctrl+C can interrupt long-running code.
void theos_mphal_poll_interrupt(void);
#define MICROPY_INTERNAL_EVENT_HOOK do { theos_mphal_poll_interrupt(); } while (0)

#endif // MICROPY_INCLUDED_THEOS_MPCONFIGPORT_H
