#ifndef _KMEM_H
#define _KMEM_H

#include <stdint.h>
#include <stddef.h>

#define MEM_STATE_USED      1
#define MEM_STATE_AVALIABLE 2

#define KMEM_HEAP_SIZE      4096

typedef struct malloc_header
{
    uint16_t state;                             // The state of the current memory frame.
    size_t size;                              // Size in bytes.
    struct malloc_header* prev_malloc_header; // The previous malloc header.
} malloc_header_t;

void kmem_init(uint64_t heap_start);

void* kmalloc(size_t size);
void* krealloc(void* ptr, size_t new_size);

void kfree(void* ptr);

#endif