#include <CPU/IO.h>

void IO_outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %1, %0" : : "d"(port), "a"(value));
}

void IO_outl(uint16_t port, uint32_t value)
{
    __asm__ __volatile__("outl %1, %0" : : "d"(port), "a"(value));
}

uint8_t IO_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}

uint32_t IO_inl(uint16_t port)
{
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}