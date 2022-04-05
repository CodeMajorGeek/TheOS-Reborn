#include <Memory/KMem.h>

#include <string.h>

static void* KMEM_heap_start;

void kmem_init(uint64_t heap_start)
{
    KMEM_heap_start = (void*) heap_start;

    malloc_header_t* first_malloc_header = (malloc_header_t*) heap_start;
    first_malloc_header->state = MEM_STATE_AVALIABLE;
    first_malloc_header->size = KMEM_HEAP_SIZE - sizeof (malloc_header_t);
    first_malloc_header->prev_malloc_header = 0;

    printf("Kmem heap start at = 0x%H\n", first_malloc_header);
}

void* kmalloc(size_t size)
{
    if (size <= 0)
        return (void*) NULL;

    malloc_header_t* malloc_header = (malloc_header_t*) KMEM_heap_start;
    while (malloc_header < (malloc_header_t*) ((KMEM_heap_start + KMEM_HEAP_SIZE) - sizeof (malloc_header_t)))
    {
        if (malloc_header->state != MEM_STATE_USED)
        {
            if (malloc_header->size == size)
            {
                malloc_header->state = MEM_STATE_USED;

                return malloc_header + sizeof (malloc_header_t);
            }
            else if (malloc_header->size > size + (sizeof (malloc_header_t) + 1))
            {
                malloc_header_t* new_bottom_malloc = (malloc_header_t*) malloc_header + size + sizeof (malloc_header_t);
                new_bottom_malloc->state = MEM_STATE_AVALIABLE;
                new_bottom_malloc->size = malloc_header->size - (size + sizeof (malloc_header_t));
                new_bottom_malloc->prev_malloc_header = (struct malloc_header*) malloc_header;

                malloc_header->state = MEM_STATE_USED;
                malloc_header->size = size;
                
                return malloc_header + sizeof (malloc_header_t);
            }
        }

        malloc_header += malloc_header->size + sizeof(malloc_header_t);
    }

    return (void*) NULL;
}

void* krealloc(void* ptr, size_t new_size)
{
    malloc_header_t* malloc_header = (malloc_header_t*) ptr - sizeof (malloc_header_t);

    /* Lazy realloc implementation, TODO: maybe tinkering on a better one :^). */
    void* new_ptr = kmalloc(new_size);
    memcpy(new_ptr, ptr, malloc_header->size);

    kfree(ptr);

    return new_ptr;
}

void kfree(void* ptr)
{
    malloc_header_t* malloc_header = (malloc_header_t*) ptr - sizeof (malloc_header_t);
    
    if (malloc_header->state == MEM_STATE_AVALIABLE)
        return;

    malloc_header_t* prev_malloc_header = ((malloc_header_t*) &malloc_header->prev_malloc_header);
    malloc_header_t* next_malloc_header = malloc_header + malloc_header->size + sizeof (malloc_header_t);

    if (next_malloc_header->state == MEM_STATE_AVALIABLE)
        malloc_header->size += next_malloc_header->size + sizeof (malloc_header_t);

    if (prev_malloc_header != (malloc_header_t*) NULL && prev_malloc_header->state == MEM_STATE_AVALIABLE)
    {
        prev_malloc_header->size += malloc_header->size + sizeof (malloc_header_t);
        malloc_header = prev_malloc_header;
    }

    malloc_header->state = MEM_STATE_AVALIABLE;
}