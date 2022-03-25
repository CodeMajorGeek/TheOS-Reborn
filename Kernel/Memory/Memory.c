#include <Memory/Memory.h>

static uint32_t mem_upper_size;

static uint64_t* mem_upper_offset;
static uint64_t* mem_upper_end;

static uint64_t* mem_last_addr; 

void kmem_init(uint32_t mem_lower, uint32_t mem_upper)
{
    mem_upper_offset = mem_lower * 1024;    // The end of the lower memory address where upper memory start.
    mem_upper_size = mem_upper * 1024;      // Total upper memory size in bytes.

    mem_upper_end = mem_upper_offset + mem_upper_size;

    malloc_header_t* first_malloc_header = (malloc_header_t*) mem_upper_offset;
    first_malloc_header->state = MEM_STATE_AVALIABLE;
    first_malloc_header->size = mem_upper_size - sizeof (malloc_header_t);
    first_malloc_header->prev_malloc_header = 0;

    mem_last_addr = first_malloc_header + sizeof (malloc_header_t);
}

void* kmalloc(uint32_t size)
{
    if (size <= 0)
        return -1;

    malloc_header_t* malloc_header = (malloc_header_t*) mem_upper_offset;
    while (malloc_header < mem_upper_end - sizeof (malloc_header_t))
    {
        printf("Malloc ptr: %H\n", malloc_header);
        printf("Malloc size: %d\n", malloc_header->size);
        printf("Malloc state: %d\n", malloc_header->state);
        if (malloc_header->state == MEM_STATE_AVALIABLE)
        {
            if (malloc_header->size == size)
            {
                malloc_header->state = MEM_STATE_USED;

                return malloc_header + sizeof (malloc_header_t);
            }
            else if (malloc_header->size > size + (sizeof (malloc_header_t) + 1))
            {
                malloc_header_t* new_bottom_malloc = (malloc_header_t*) 0xA0500;
                new_bottom_malloc->state = MEM_STATE_AVALIABLE;
                new_bottom_malloc->size = malloc_header->size - (size + sizeof (malloc_header_t));
                new_bottom_malloc->prev_malloc_header = malloc_header;
                printf("New btm malloc ptr: %H\n", new_bottom_malloc);
                printf("New btm malloc size: %d\n", (size + sizeof (malloc_header_t)));

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
    malloc_header_t* malloc_header = (malloc_header_t*) (ptr - sizeof (malloc_header_t));
    
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