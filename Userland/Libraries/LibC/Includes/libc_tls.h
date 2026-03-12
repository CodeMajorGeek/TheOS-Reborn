#ifndef _LIBC_TLS_H
#define _LIBC_TLS_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#endif

#define LIBC_TLS_PAGE_SIZE 4096U
#define LIBC_TLS_MIN_ALIGN 16U
#define LIBC_TLS_MODULE_MAIN 1U
#define LIBC_TLS_MAX_MODULES 64U
#define LIBC_TLS_INITIAL_DTV_CAPACITY 8U

#ifndef __ASSEMBLER__
typedef struct libc_tls_dtv_entry
{
    void* module_tp;
    void* map_base;
    size_t map_size;
    uint8_t owns_block;
    uint8_t reserved[7];
} libc_tls_dtv_entry_t;

typedef struct libc_tls_tcb
{
    void* self;
    void* static_tls_base;
    size_t static_tls_size;
    size_t static_tls_align;
    libc_tls_dtv_entry_t* dtv_entries;
    size_t dtv_capacity;
    size_t dtv_map_size;
    uint64_t dtv_generation;
} libc_tls_tcb_t;

typedef struct libc_tls_region
{
    void* mapping_base;
    size_t mapping_size;
    uintptr_t thread_pointer;
} libc_tls_region_t;

typedef struct libc_tls_index
{
    uint64_t module_id;
    uint64_t offset;
} libc_tls_index_t;

int __libc_tls_init_main(void);
int __libc_tls_create(libc_tls_region_t* out_region);
void __libc_tls_destroy(const libc_tls_region_t* region);
int __libc_tls_activate(uintptr_t thread_pointer);
uintptr_t __libc_tls_current_tp(void);
int __libc_tls_module_register(const void* init_image,
                               size_t init_size,
                               size_t image_size,
                               size_t align,
                               size_t* out_module_id);
int __libc_tls_module_unregister(size_t module_id);
void* __tls_get_addr(const libc_tls_index_t* index);
#endif

#endif
