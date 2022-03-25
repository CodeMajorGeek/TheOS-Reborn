#ifndef _MEMORY_H
#define _MEMORY_H

#include <stdint.h>
#include <string.h>

#define MEM_LOWER_OFFSET    0
#define MEM_UPPER_OFFSET    (1 * 1024 * 1024)   // (1 MiB)

#define MEM_STATE_USED      1
#define MEM_STATE_AVALIABLE 2

typedef struct malloc_header
{
    uint16_t state;                             // The state of the current memory frame.
    uint32_t size;                              // Size in bytes.
    struct malloc_header_t* prev_malloc_header;        // The previous malloc header.
} malloc_header_t;

void kmem_init(uint32_t, uint32_t);

void* kmalloc(uint32_t);
void kfree(void*);


#endif