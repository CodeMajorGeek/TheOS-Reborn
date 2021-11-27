#ifndef _LOGGER_H
#define _LOGGER_H

#include <stdlib.h>

#ifdef __USE_QEMU
#include <Device/COM.h>
#else
#include <Device/TTY.h>
#endif

#ifdef __USE_QEMU
#define LOGGER_COM_PORT COM1
#endif

#define LEVEL_COUNT 4

enum level {
    KDEBUG  = 0,
    KINFO   = 1,
    KWARN   = 2,
    KPANIC  = 3
};

static const char* level_messages[LEVEL_COUNT] =
{
    "[DEBUG] ",
    "[INFO] ",
    "[WARNING] ",
    "[PANIC] "
};

void logger_init(void);

void kputc(int level, char c);
void kputs(int level, const char* str);

#endif