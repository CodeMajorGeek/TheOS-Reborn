#ifndef _IO_H
#define _IO_H

#include <stdint.h>

#define UNUSED_ADDRESS  0x80

static inline void IO_outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__ ("outb %0, %1" : : "a" (value), "d" (port));
}


static inline void IO_outl(uint16_t port, uint32_t value)
{
    __asm__ __volatile__ ("outl %0, %1" : : "a" (value), "Nd" (port));
}

static inline uint8_t IO_inb(uint16_t port)
{
    uint8_t result;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (result) : "Nd" (port));
    return result;
}


static inline uint32_t IO_inl(uint16_t port)
{
    uint32_t result;
    __asm__ __volatile__ ("inl %1, %0" : "=a" (result) : "Nd" (port));
    return result;
}


static inline void IO_wait(void)
{
    IO_outb(UNUSED_ADDRESS, 0);
}

#endif