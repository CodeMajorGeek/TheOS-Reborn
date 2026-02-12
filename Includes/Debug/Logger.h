#ifndef _LOGGER_H
#define _LOGGER_H

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

void kprintf(int level, const char* restrict format, ...);

#endif
