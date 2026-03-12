#ifndef _KMEM_PRIVATE_H
#define _KMEM_PRIVATE_H

#include <Memory/KMem.h>

static void* kmalloc_nolock(size_t size);
static void kfree_nolock(void* ptr);

#endif
