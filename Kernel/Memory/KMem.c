#include <Memory/KMem.h>

#include <string.h>
#include <stdint.h>

static void* KMEM_heap_start;
static size_t KMEM_heap_size;

static size_t kmem_align(size_t size)
{
    const size_t alignment = sizeof(uintptr_t);
    return (size + (alignment - 1)) & ~(alignment - 1);
}

static uint8_t* kmem_heap_end(void)
{
    return (uint8_t*) KMEM_heap_start + KMEM_heap_size;
}

static malloc_header_t* kmem_next_header(malloc_header_t* header)
{
    return (malloc_header_t*) ((uint8_t*) header + sizeof (malloc_header_t) + header->size);
}

static int kmem_header_is_valid(malloc_header_t* header)
{
    return (uint8_t*) header + sizeof (malloc_header_t) <= kmem_heap_end();
}

void kmem_init(uint64_t heap_start, size_t heap_size)
{
    KMEM_heap_start = (void*) heap_start;
    KMEM_heap_size = kmem_align(heap_size);

    if (KMEM_heap_size <= sizeof (malloc_header_t))
    {
        KMEM_heap_start = NULL;
        KMEM_heap_size = 0;
        return;
    }

    malloc_header_t* first_malloc_header = (malloc_header_t*) KMEM_heap_start;
    first_malloc_header->state = MEM_STATE_AVALIABLE;
    first_malloc_header->size = KMEM_heap_size - sizeof (malloc_header_t);
    first_malloc_header->prev_malloc_header = NULL;
}

void* kmalloc(size_t size)
{
    if (size == 0)
        return (void*) NULL;
    if (!KMEM_heap_start || KMEM_heap_size <= sizeof (malloc_header_t))
        return (void*) NULL;

    size = kmem_align(size);

    malloc_header_t* malloc_header = (malloc_header_t*) KMEM_heap_start;
    while (kmem_header_is_valid(malloc_header))
    {
        if (malloc_header->state == MEM_STATE_AVALIABLE && malloc_header->size >= size)
        {
            size_t remaining = malloc_header->size - size;
            if (remaining > sizeof (malloc_header_t))
            {
                malloc_header_t* new_bottom_malloc =
                    (malloc_header_t*) ((uint8_t*) malloc_header + sizeof (malloc_header_t) + size);
                new_bottom_malloc->state = MEM_STATE_AVALIABLE;
                new_bottom_malloc->size = remaining - sizeof (malloc_header_t);
                new_bottom_malloc->prev_malloc_header = malloc_header;

                malloc_header_t* next = kmem_next_header(new_bottom_malloc);
                if (kmem_header_is_valid(next))
                    next->prev_malloc_header = new_bottom_malloc;

                malloc_header->size = size;
            }

            malloc_header->state = MEM_STATE_USED;
            return (void*) ((uint8_t*) malloc_header + sizeof (malloc_header_t));
        }

        malloc_header_t* next = kmem_next_header(malloc_header);
        if (!kmem_header_is_valid(next))
            break;
        malloc_header = next;
    }

    return (void*) NULL;
}

void* krealloc(void* ptr, size_t new_size)
{
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0)
    {
        kfree(ptr);
        return NULL;
    }

    malloc_header_t* malloc_header = (malloc_header_t*) ((uint8_t*) ptr - sizeof (malloc_header_t));
    if (malloc_header->size >= new_size)
        return ptr;

    void* new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, malloc_header->size);

    kfree(ptr);

    return new_ptr;
}

void kfree(void* ptr)
{
    if (!ptr)
        return;

    malloc_header_t* malloc_header = (malloc_header_t*) ((uint8_t*) ptr - sizeof (malloc_header_t));
    
    if (malloc_header->state == MEM_STATE_AVALIABLE)
        return;

    malloc_header_t* prev_malloc_header = malloc_header->prev_malloc_header;
    malloc_header_t* next_malloc_header = kmem_next_header(malloc_header);

    if (kmem_header_is_valid(next_malloc_header) && next_malloc_header->state == MEM_STATE_AVALIABLE)
        malloc_header->size += next_malloc_header->size + sizeof (malloc_header_t);

    if (prev_malloc_header != NULL && prev_malloc_header->state == MEM_STATE_AVALIABLE)
    {
        prev_malloc_header->size += malloc_header->size + sizeof (malloc_header_t);
        malloc_header = prev_malloc_header;
    }

    malloc_header_t* next = kmem_next_header(malloc_header);
    if (kmem_header_is_valid(next))
        next->prev_malloc_header = malloc_header;

    malloc_header->state = MEM_STATE_AVALIABLE;
}
