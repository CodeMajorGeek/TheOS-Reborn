#ifndef _BUFFER_H
#define _BUFFER_H

#define DEV_TYPE_MASK   0xF00000000
#define DEV_NUM_MASK    0x0FFFFFFFF
#define DEV_IDE         0
#define DEV_SATA        1

#define TODEVNUM(type, num) ((type << 28) + (DEV_NUM_MASK & num))

#endif