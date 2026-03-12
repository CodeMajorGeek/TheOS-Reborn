#ifndef _KDEBUG_H
#define _KDEBUG_H

#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define KDEBUG_COM_PORT 0x3F8  // COM1
#define KDEBUG_FILE_RAM_BUFFER_SIZE (1024U * 1024U)
#define KDEBUG_FILE_NAME_MAX 64U
#define KDEBUG_FILE_CHUNK_STACK_MAX 4096U

#ifndef THEOS_KDEBUG_LOG_SERIAL
#define THEOS_KDEBUG_LOG_SERIAL 1
#endif

#ifndef THEOS_KDEBUG_LOG_FILE
#define THEOS_KDEBUG_LOG_FILE 0
#endif

typedef struct kdebug_runtime_state
{
    bool initialized;
    bool serial_ready;
    spinlock_t lock;
#if THEOS_KDEBUG_LOG_FILE
    char file_ram[KDEBUG_FILE_RAM_BUFFER_SIZE];
    size_t file_len;
    size_t file_dropped;
    bool file_fs_ready;
#endif
} kdebug_runtime_state_t;

void kdebug_init(void);
void kdebug_putc(char c);
void kdebug_puts(const char* str);
void kdebug_printf(const char* format, ...);
void kdebug_hex(uint64_t value, int width);
void kdebug_dec(uint64_t value);
void kdebug_file_sink_ready(void);
void kdebug_file_flush(void);

#endif
