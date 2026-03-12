#ifndef _LOGGER_H
#define _LOGGER_H

#include <stdbool.h>

#ifndef THEOS_KDEBUG_LOG_SERIAL
#define THEOS_KDEBUG_LOG_SERIAL 1
#endif

#define LEVEL_COUNT 4

#ifdef __USE_QEMU
#define LOGGER_COM_PORT 0x3F8
#endif

enum level {
    KDEBUG  = 0,
    KINFO   = 1,
    KWARN   = 2,
    KPANIC  = 3
};

typedef struct logger_runtime_state
{
    bool serial_ready;
} logger_runtime_state_t;

extern const char* const logger_level_messages[LEVEL_COUNT];

void logger_init(void);

void kputc(int level, char c);
void kputs(int level, const char* str);

void kprintf(int level, const char* restrict format, ...);

#endif
