#include <Device/CMOS.h>
#include <CPU/x86.h>
#include <CPU/IO.h>

uint8_t CMOS_read(uint8_t reg)
{
    cli();
    IO_outb(CMOS_ADDRESS, reg);
    uint8_t ret = IO_inb(CMOS_DATA);
    sti();

    return ret;
}