#ifndef _MEMORY_H
#define _MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MEM_STATE_USED      1
#define MEM_STATE_AVALIABLE 2

typedef struct malloc_header
{
    uint16_t state;                             // The state of the current memory frame.
    uint32_t size;                              // Size in bytes.
    struct malloc_header* prev_malloc_header; // The previous malloc header.
} malloc_header_t;

extern void* kernel_heap_bottom;
extern void* kernel_heap_top;

void kmem_init(void);

void* kmalloc(size_t);
void kfree(void*);


#endif