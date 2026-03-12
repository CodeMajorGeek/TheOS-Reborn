#include <stdlib.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define LIBC_HEAP_ALIGN             16U
#define LIBC_HEAP_GROW_CHUNK        (64U * 1024U)
#define LIBC_HEAP_SPIN_BEFORE_YIELD 256U
#define LIBC_HEAP_ALIGNED_MAGIC     0xA11C0CDE5A5A13D5ULL

typedef struct libc_heap_block
{
    size_t size;
    struct libc_heap_block* next;
    struct libc_heap_block* prev;
    int free;
} libc_heap_block_t;

typedef struct libc_heap_aligned_meta
{
    uint64_t magic;
    void* base_ptr;
} libc_heap_aligned_meta_t;

#define LIBC_HEAP_HEADER_SIZE \
    (((sizeof(libc_heap_block_t) + LIBC_HEAP_ALIGN - 1U) / LIBC_HEAP_ALIGN) * LIBC_HEAP_ALIGN)

static libc_heap_block_t* LibC_heap_head = NULL;
static libc_heap_block_t* LibC_heap_tail = NULL;
static volatile uint8_t LibC_heap_lock = 0;

static void heap_lock(void)
{
    unsigned int spins = 0U;
    while (__atomic_test_and_set(&LibC_heap_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_heap_lock, __ATOMIC_RELAXED) != 0U)
        {
            spins++;
            if (spins >= LIBC_HEAP_SPIN_BEFORE_YIELD)
            {
                spins = 0U;
                (void) sched_yield();
            }
        }
    }
}

static void heap_unlock(void)
{
    __atomic_clear(&LibC_heap_lock, __ATOMIC_RELEASE);
}

static size_t heap_align_up(size_t size)
{
    return (size + (LIBC_HEAP_ALIGN - 1U)) & ~(LIBC_HEAP_ALIGN - 1U);
}

static uintptr_t heap_align_up_uintptr(uintptr_t value, size_t align)
{
    uintptr_t align_u = (uintptr_t) align;
    return (value + (align_u - 1U)) & ~(align_u - 1U);
}

static bool heap_add_overflow(size_t a, size_t b, size_t* out)
{
    if (a > SIZE_MAX - b)
        return true;
    if (out)
        *out = a + b;
    return false;
}

static libc_heap_block_t* heap_block_from_ptr(void* ptr)
{
    return (libc_heap_block_t*) ((uint8_t*) ptr - LIBC_HEAP_HEADER_SIZE);
}

static void* heap_ptr_from_block(libc_heap_block_t* block)
{
    return (void*) ((uint8_t*) block + LIBC_HEAP_HEADER_SIZE);
}

static libc_heap_block_t* heap_find_block_by_payload(void* ptr)
{
    for (libc_heap_block_t* it = LibC_heap_head; it; it = it->next)
    {
        if (heap_ptr_from_block(it) == ptr)
            return it;
    }

    return NULL;
}

static void heap_split_block(libc_heap_block_t* block, size_t wanted)
{
    if (!block || block->size < wanted)
        return;

    size_t needed = 0;
    if (heap_add_overflow(wanted, LIBC_HEAP_HEADER_SIZE + LIBC_HEAP_ALIGN, &needed))
        return;
    if (block->size < needed)
        return;

    uint8_t* raw = (uint8_t*) block;
    libc_heap_block_t* split = (libc_heap_block_t*) (raw + LIBC_HEAP_HEADER_SIZE + wanted);
    split->size = block->size - wanted - LIBC_HEAP_HEADER_SIZE;
    split->next = block->next;
    split->prev = block;
    split->free = 1;

    if (block->next)
        block->next->prev = split;
    else
        LibC_heap_tail = split;

    block->next = split;
    block->size = wanted;
}

static void heap_coalesce_forward(libc_heap_block_t* block)
{
    while (block && block->next && block->next->free)
    {
        libc_heap_block_t* next = block->next;
        size_t merged = 0;
        if (heap_add_overflow(block->size, LIBC_HEAP_HEADER_SIZE, &merged) ||
            heap_add_overflow(merged, next->size, &merged))
            return;

        block->size = merged;
        block->next = next->next;
        if (next->next)
            next->next->prev = block;
        else
            LibC_heap_tail = block;
    }
}

static void heap_trim_tail(void)
{
    while (LibC_heap_tail && LibC_heap_tail->free)
    {
        size_t release_size = 0;
        if (heap_add_overflow(LibC_heap_tail->size, LIBC_HEAP_HEADER_SIZE, &release_size))
            return;
        if (release_size > (size_t) INTPTR_MAX)
            return;

        if (sbrk(-((intptr_t) release_size)) == (void*) -1)
            return;

        libc_heap_block_t* prev = LibC_heap_tail->prev;
        if (prev)
            prev->next = NULL;
        else
            LibC_heap_head = NULL;
        LibC_heap_tail = prev;
    }
}

static libc_heap_block_t* heap_find_free(size_t wanted)
{
    for (libc_heap_block_t* it = LibC_heap_head; it; it = it->next)
    {
        if (it->free && it->size >= wanted)
            return it;
    }

    return NULL;
}

static libc_heap_block_t* heap_request_more(size_t wanted)
{
    size_t payload = wanted;
    if (payload < (LIBC_HEAP_GROW_CHUNK - LIBC_HEAP_HEADER_SIZE))
        payload = LIBC_HEAP_GROW_CHUNK - LIBC_HEAP_HEADER_SIZE;
    payload = heap_align_up(payload);

    size_t total = 0;
    if (heap_add_overflow(payload, LIBC_HEAP_HEADER_SIZE, &total) ||
        total > (size_t) INTPTR_MAX)
    {
        errno = ENOMEM;
        return NULL;
    }

    void* raw = sbrk((intptr_t) total);
    if (raw == (void*) -1)
        return NULL;

    libc_heap_block_t* block = (libc_heap_block_t*) raw;
    block->size = payload;
    block->next = NULL;
    block->prev = LibC_heap_tail;
    block->free = 1;

    if (LibC_heap_tail)
        LibC_heap_tail->next = block;
    else
        LibC_heap_head = block;
    LibC_heap_tail = block;
    return block;
}

static bool heap_extract_aligned_base_unlocked(void* ptr, void** out_base, size_t* out_usable)
{
    if (!ptr)
        return false;

    uintptr_t user = (uintptr_t) ptr;
    if (user < sizeof(libc_heap_aligned_meta_t))
        return false;

    libc_heap_aligned_meta_t* meta =
        (libc_heap_aligned_meta_t*) (user - sizeof(libc_heap_aligned_meta_t));
    if (meta->magic != LIBC_HEAP_ALIGNED_MAGIC || !meta->base_ptr)
        return false;

    libc_heap_block_t* base_block = heap_find_block_by_payload(meta->base_ptr);
    if (!base_block || base_block->free)
        return false;

    uintptr_t base = (uintptr_t) meta->base_ptr;
    if (user < base)
        return false;
    if (user == base)
        return false;

    size_t offset = (size_t) (user - base);
    if (offset < sizeof(libc_heap_aligned_meta_t))
        return false;
    if (offset > base_block->size)
        return false;

    if (out_base)
        *out_base = meta->base_ptr;
    if (out_usable)
        *out_usable = base_block->size - offset;
    return true;
}

static void* heap_malloc_unlocked(size_t size)
{
    if (size == 0U)
        size = 1U;
    size = heap_align_up(size);

    libc_heap_block_t* block = heap_find_free(size);
    if (!block)
    {
        block = heap_request_more(size);
        if (!block)
            return NULL;
    }

    heap_split_block(block, size);
    block->free = 0;
    return heap_ptr_from_block(block);
}

static void heap_free_unlocked(void* ptr)
{
    if (!ptr)
        return;

    void* base_ptr = NULL;
    if (heap_extract_aligned_base_unlocked(ptr, &base_ptr, NULL))
    {
        libc_heap_aligned_meta_t* meta =
            (libc_heap_aligned_meta_t*) ((uintptr_t) ptr - sizeof(libc_heap_aligned_meta_t));
        meta->magic = 0U;
        meta->base_ptr = NULL;
        heap_free_unlocked(base_ptr);
        return;
    }

    libc_heap_block_t* block = heap_block_from_ptr(ptr);
    block->free = 1;

    heap_coalesce_forward(block);
    if (block->prev && block->prev->free)
    {
        block = block->prev;
        heap_coalesce_forward(block);
    }

    if (block->next == NULL)
        heap_trim_tail();
}

static void* heap_realloc_unlocked(void* ptr, size_t size)
{
    if (!ptr)
        return heap_malloc_unlocked(size);

    if (size == 0U)
    {
        heap_free_unlocked(ptr);
        return NULL;
    }

    size = heap_align_up(size);

    void* aligned_base = NULL;
    size_t aligned_usable = 0U;
    if (heap_extract_aligned_base_unlocked(ptr, &aligned_base, &aligned_usable))
    {
        if (aligned_usable >= size)
            return ptr;

        void* new_ptr = heap_malloc_unlocked(size);
        if (!new_ptr)
            return NULL;

        memcpy(new_ptr, ptr, aligned_usable);
        heap_free_unlocked(ptr);
        return new_ptr;
    }

    libc_heap_block_t* block = heap_block_from_ptr(ptr);

    if (block->size >= size)
    {
        heap_split_block(block, size);
        return ptr;
    }

    if (block->next && block->next->free)
    {
        heap_coalesce_forward(block);
        if (block->size >= size)
        {
            heap_split_block(block, size);
            block->free = 0;
            return ptr;
        }
    }

    void* new_ptr = heap_malloc_unlocked(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, block->size);
    heap_free_unlocked(ptr);
    return new_ptr;
}

static int heap_posix_memalign_unlocked(void** memptr, size_t alignment, size_t size)
{
    if (!memptr)
        return EINVAL;

    if (alignment < sizeof(void*))
        return EINVAL;
    if ((alignment & (alignment - 1U)) != 0U)
        return EINVAL;

    if (alignment <= LIBC_HEAP_ALIGN)
    {
        void* ptr = heap_malloc_unlocked(size);
        if (!ptr)
            return ENOMEM;
        *memptr = ptr;
        return 0;
    }

    size_t prefix = 0U;
    if (heap_add_overflow(alignment - 1U, sizeof(libc_heap_aligned_meta_t), &prefix))
        return ENOMEM;

    size_t request_size = 0U;
    if (heap_add_overflow(size, prefix, &request_size))
        return ENOMEM;

    void* base_ptr = heap_malloc_unlocked(request_size);
    if (!base_ptr)
        return ENOMEM;

    uintptr_t user_start = (uintptr_t) base_ptr + sizeof(libc_heap_aligned_meta_t);
    uintptr_t aligned_user = heap_align_up_uintptr(user_start, alignment);

    libc_heap_aligned_meta_t* meta =
        (libc_heap_aligned_meta_t*) (aligned_user - sizeof(libc_heap_aligned_meta_t));
    meta->magic = LIBC_HEAP_ALIGNED_MAGIC;
    meta->base_ptr = base_ptr;

    *memptr = (void*) aligned_user;
    return 0;
}

void* malloc(size_t size)
{
    heap_lock();
    void* ptr = heap_malloc_unlocked(size);
    heap_unlock();
    return ptr;
}

void free(void* ptr)
{
    heap_lock();
    heap_free_unlocked(ptr);
    heap_unlock();
}

void* calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0U || size == 0U)
    {
        nmemb = 1U;
        size = 1U;
    }

    if (nmemb > (SIZE_MAX / size))
    {
        errno = ENOMEM;
        return NULL;
    }

    size_t total = nmemb * size;

    heap_lock();
    void* ptr = heap_malloc_unlocked(total);
    if (ptr)
        memset(ptr, 0, total);
    heap_unlock();
    return ptr;
}

void* realloc(void* ptr, size_t size)
{
    heap_lock();
    void* out = heap_realloc_unlocked(ptr, size);
    heap_unlock();
    return out;
}

int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    heap_lock();
    int rc = heap_posix_memalign_unlocked(memptr, alignment, size);
    heap_unlock();
    return rc;
}

void* aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U || (size % alignment) != 0U)
    {
        errno = EINVAL;
        return NULL;
    }

    void* ptr = NULL;
    heap_lock();
    int rc = heap_posix_memalign_unlocked(&ptr, alignment, size);
    heap_unlock();

    if (rc != 0)
    {
        errno = rc;
        return NULL;
    }

    return ptr;
}
