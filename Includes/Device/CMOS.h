#ifndef _CMOS_H
#define _CMOS_H

#define CMOS_ADDRESS    0x70
#define CMOS_DATA       0x71

#include <stdint.h>

uint8_t CMOS_read(uint8_t reg);

#endif