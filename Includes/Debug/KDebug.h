#ifndef _KDEBUG_H
#define _KDEBUG_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define KDEBUG_COM_PORT 0x3F8  // COM1

void kdebug_init(void);
void kdebug_putc(char c);
void kdebug_puts(const char* str);
void kdebug_printf(const char* format, ...);
void kdebug_hex(uint64_t value, int width);
void kdebug_dec(uint64_t value);

#endif

