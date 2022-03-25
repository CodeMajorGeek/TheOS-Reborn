#include <Memory/Memory.h>

static uint32_t heap_size; // In bytes.

static uint64_t* mem_last_addr; 

void kmem_init(void)
{
    heap_size = kernel_heap_bottom - kernel_heap_top;

    malloc_header_t* first_malloc_header = (malloc_header_t*) kernel_heap_top;
    first_malloc_header->state = MEM_STATE_AVALIABLE;
    first_malloc_header->size = heap_size - sizeof (malloc_header_t);
    first_malloc_header->prev_malloc_header = 0;

    mem_last_addr = first_malloc_header + sizeof (malloc_header_t);
}

void* kmalloc(uint32_t size)
{
    if (size <= 0)
        return -1;

    malloc_header_t* malloc_header = (malloc_header_t*) kernel_heap_top;
    while (malloc_header < kernel_heap_bottom - sizeof (malloc_header_t))
    {
        if (malloc_header->state == MEM_STATE_AVALIABLE)
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
                new_bottom_malloc->prev_malloc_header = malloc_header;

                malloc_header->state = MEM_STATE_USED;
                malloc_header->size = size;
                
                return malloc_header + sizeof (malloc_header_t);
            }
        }

        malloc_header += malloc_header->size + sizeof(malloc_header_t);
    }

    return -1;
}

void kfree(void* ptr)
{
    malloc_header_t* malloc_header = (malloc_header_t*) ptr - sizeof (malloc_header_t);
    
    if (malloc_header->state == MEM_STATE_AVALIABLE)
        return;

    malloc_header_t* prev_malloc_header = malloc_header->prev_malloc_header;
    malloc_header_t* next_malloc_header = malloc_header + malloc_header->size + sizeof (malloc_header_t);

    if (next_malloc_header->state == MEM_STATE_AVALIABLE)
        malloc_header->size += next_malloc_header->size + sizeof (malloc_header_t);

    if (prev_malloc_header != -1 && prev_malloc_header->state == MEM_STATE_AVALIABLE)
    {
        prev_malloc_header->size += malloc_header->size + sizeof (malloc_header_t);
        malloc_header = prev_malloc_header;
    }

    malloc_header->state = MEM_STATE_AVALIABLE;
}