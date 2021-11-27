#ifndef _COM_H
#define _COM_H

#define COM_PORT1 0x3F8
#define COM_PORT2 0x2F8
#define COM_PORT3 0x3E8
#define COM_PORT4 0x2E8

#define COM_PORT COM_PORT1

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include <CPU/IO.h>

bool COM_init(void);

bool COM_transmit_empty();
void COM_putc(char c);
void COM_write(const char* str, size_t len);
void COM_puts(const char* str);

#endif