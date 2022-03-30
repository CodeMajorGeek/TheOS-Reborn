#ifndef _IO_H
#define _IO_H

#include <stdint.h>

#define UNUSED_ADDRESS  0x80

void IO_outb(uint16_t port, uint8_t value);
void IO_outl(uint16_t port, uint32_t value);

uint8_t IO_inb(uint16_t port);
uint32_t IO_inl(uint16_t port);


static inline void IO_wait(void)
{
    IO_outb(UNUSED_ADDRESS, 0);
}

#endif