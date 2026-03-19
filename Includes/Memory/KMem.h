#ifndef _KMEM_H
#define _KMEM_H

#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MEM_STATE_USED      1
#define MEM_STATE_AVAILABLE 2
#define MEM_STATE_AVALIABLE MEM_STATE_AVAILABLE

#define KMEM_HEAP_SIZE      (16 * 1024 * 1024)

typedef struct malloc_header
{
    uint16_t state;                             // The state of the current memory frame.
    size_t size;                                // Size in bytes.
    struct malloc_header* prev_malloc_header;   // The previous malloc header.
} malloc_header_t;

typedef struct KMEM_runtime_state
{
    void* heap_start;
    size_t heap_size;
    spinlock_t lock;
    bool lock_ready;
} KMEM_runtime_state_t;

void kmem_init(uint64_t heap_start, size_t heap_size);

void* kmalloc(size_t size);
void* krealloc(void* ptr, size_t new_size);

void kfree(void* ptr);

#endif
