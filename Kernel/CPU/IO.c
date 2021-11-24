#include <CPU/IO.h>

void IO_outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %1, %0" : : "d"(port), "a"(value));
}

uint8_t IO_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "d"(port));
    return ret;
}