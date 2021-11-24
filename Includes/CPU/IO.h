#ifndef _IO_H
#define _IO_H

#include <stdint.h>

void IO_outb(uint16_t port, uint8_t value);

uint8_t IO_inb(uint16_t port);

#endif