#include <dlfcn.h>

#include <errno.h>
#include <fcntl.h>
#include <libc_elf.h>
#include <libc_tls.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIBC_DL_PAGE_SIZE         4096UL
#define LIBC_DL_PATH_MAX          256U
#define LIBC_DL_ERROR_MAX         256U
#define LIBC_DL_MAX_SEGMENTS      16U
#define LIBC_DL_MAX_NEEDED        32U
#define LIBC_DL_MAX_SYMBOL_LOOKUP 256U
#define LIBC_DL_SPIN_BEFORE_YIELD 256U

typedef struct libc_dl_segment
{
    uintptr_t addr;
    size_t size;
    int prot;
} libc_dl_segment_t;

typedef struct libc_dl_module
{
    struct libc_dl_module* next;

    char path[LIBC_DL_PATH_MAX];

    void* map_base;
    size_t map_size;
    uintptr_t load_bias;
    uint32_t ref_count;
    bool global_scope;
    bool initialized;

    libc_elf64_dyn_t* dynamic;
    size_t dynamic_count;

    const char* strtab;
    size_t strtab_size;
    libc_elf64_sym_t* symtab;
    size_t symbol_count;
    libc_elf64_rela_t* rela;
    size_t rela_count;
    libc_elf64_rela_t* plt_rela;
    size_t plt_rela_count;

    uintptr_t init_func;
    uintptr_t fini_func;
    uintptr_t init_array;
    size_t init_array_count;
    uintptr_t fini_array;
    size_t fini_array_count;

    uint64_t tls_vaddr;
    size_t tls_filesz;
    size_t tls_memsz;
    size_t tls_align;
    size_t tls_module_id;

    libc_dl_segment_t segments[LIBC_DL_MAX_SEGMENTS];
    size_t segment_count;

    uint32_t needed_offsets[LIBC_DL_MAX_NEEDED];
    size_t needed_count;
    struct libc_dl_module* needed_modules[LIBC_DL_MAX_NEEDED];
    size_t needed_module_count;
} libc_dl_module_t;

static libc_dl_module_t* LibC_dl_modules = NULL;
static volatile uint8_t LibC_dl_lock_byte = 0U;
static __thread char LibC_dl_error_message[LIBC_DL_ERROR_MAX];
static __thread bool LibC_dl_error_ready = false;
static int LibC_dl_main_program_sentinel = 0;

static uintptr_t LibC_dl_align_down_page(uintptr_t value)
{
    return value & ~(uintptr_t) (LIBC_DL_PAGE_SIZE - 1U);
}

static uintptr_t LibC_dl_align_up_page(uintptr_t value)
{
    if (value > UINTPTR_MAX - (LIBC_DL_PAGE_SIZE - 1U))
        return 0U;
    return (value + (LIBC_DL_PAGE_SIZE - 1U)) & ~(uintptr_t) (LIBC_DL_PAGE_SIZE - 1U);
}

static void LibC_dl_lock(void)
{
    unsigned int spins = 0U;
    while (__atomic_test_and_set(&LibC_dl_lock_byte, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&LibC_dl_lock_byte, __ATOMIC_RELAXED) != 0U)
        {
            spins++;
            if (spins >= LIBC_DL_SPIN_BEFORE_YIELD)
            {
                spins = 0U;
                (void) sched_yield();
            }
        }
    }
}

static void LibC_dl_unlock(void)
{
    __atomic_clear(&LibC_dl_lock_byte, __ATOMIC_RELEASE);
}

static void LibC_dl_clear_error(void)
{
    LibC_dl_error_ready = false;
    LibC_dl_error_message[0] = '\0';
}

static void LibC_dl_set_error(const char* format, ...)
{
    if (!format)
    {
        LibC_dl_error_ready = true;
        LibC_dl_error_message[0] = '\0';
        return;
    }

    va_list ap;
    va_start(ap, format);
    (void) __printf(LibC_dl_error_message, sizeof(LibC_dl_error_message), format, ap);
    va_end(ap);
    LibC_dl_error_ready = true;
}

static bool LibC_dl_path_has_slash(const char* path)
{
    if (!path)
        return false;

    while (*path != '\0')
    {
        if (*path == '/')
            return true;
        path++;
    }

    return false;
}

static bool LibC_dl_copy_path(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0U || !src)
        return false;

    size_t src_len = strlen(src);
    if (src_len + 1U > dst_size)
        return false;

    memcpy(dst, src, src_len + 1U);
    return true;
}

static bool LibC_dl_format_join(char* out_path,
                                size_t out_path_size,
                                const char* left,
                                const char* right)
{
    if (!out_path || out_path_size == 0U || !left || !right)
        return false;

    int written = snprintf(out_path, out_path_size, "%s/%s", left, right);
    if (written < 0)
        return false;
    return (size_t) written < out_path_size;
}

static bool LibC_dl_parent_dir(const char* path, char* out_dir, size_t out_dir_size)
{
    if (!path || !out_dir || out_dir_size == 0U)
        return false;

    const char* slash = NULL;
    for (const char* cursor = path; *cursor != '\0'; cursor++)
    {
        if (*cursor == '/')
            slash = cursor;
    }

    if (!slash)
        return false;

    size_t len = (size_t) (slash - path);
    if (len == 0U)
        len = 1U;
    if (len + 1U > out_dir_size)
        return false;

    memcpy(out_dir, path, len);
    out_dir[len] = '\0';
    return true;
}

static bool LibC_dl_access_readable(const char* path)
{
    if (!path || path[0] == '\0')
        return false;
    return access(path, R_OK) == 0;
}

static bool LibC_dl_resolve_path(const char* request_path,
                                 const char* parent_dir,
                                 char* out_path,
                                 size_t out_path_size)
{
    if (!request_path || !out_path || out_path_size == 0U)
        return false;

    if (LibC_dl_path_has_slash(request_path))
    {
        if (!LibC_dl_copy_path(out_path, out_path_size, request_path))
            return false;
        return LibC_dl_access_readable(out_path);
    }

    char candidate[LIBC_DL_PATH_MAX];
    if (parent_dir && parent_dir[0] != '\0')
    {
        if (LibC_dl_format_join(candidate, sizeof(candidate), parent_dir, request_path) &&
            LibC_dl_access_readable(candidate))
        {
            return LibC_dl_copy_path(out_path, out_path_size, candidate);
        }
    }

    if (LibC_dl_format_join(candidate, sizeof(candidate), "/lib", request_path) &&
        LibC_dl_access_readable(candidate))
    {
        return LibC_dl_copy_path(out_path, out_path_size, candidate);
    }

    if (LibC_dl_format_join(candidate, sizeof(candidate), "/bin", request_path) &&
        LibC_dl_access_readable(candidate))
    {
        return LibC_dl_copy_path(out_path, out_path_size, candidate);
    }

    if (LibC_dl_access_readable(request_path))
        return LibC_dl_copy_path(out_path, out_path_size, request_path);

    return false;
}

static bool LibC_dl_read_file(const char* path, uint8_t** out_data, size_t* out_size)
{
    if (!path || !out_data || !out_size)
        return false;

    *out_data = NULL;
    *out_size = 0U;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0)
    {
        (void) close(fd);
        return false;
    }

    size_t total_size = (size_t) st.st_size;
    uint8_t* image = NULL;
    if (total_size != 0U)
    {
        image = (uint8_t*) malloc(total_size);
        if (!image)
        {
            (void) close(fd);
            return false;
        }
    }
    else
    {
        image = (uint8_t*) malloc(1U);
        if (!image)
        {
            (void) close(fd);
            return false;
        }
    }

    size_t offset = 0U;
    while (offset < total_size)
    {
        ssize_t rc = read(fd, image + offset, total_size - offset);
        if (rc <= 0)
        {
            free(image);
            (void) close(fd);
            return false;
        }
        offset += (size_t) rc;
    }

    (void) close(fd);
    *out_data = image;
    *out_size = total_size;
    return true;
}

static bool LibC_dl_addr_range_valid(const libc_dl_module_t* module, uintptr_t addr, size_t size)
{
    if (!module || !module->map_base || module->map_size == 0U || size == 0U)
        return false;

    uintptr_t map_start = (uintptr_t) module->map_base;
    uintptr_t map_end = map_start + module->map_size;
    if (map_end < map_start)
        return false;
    if (addr < map_start || addr > map_end)
        return false;
    if (size > (size_t) (map_end - addr))
        return false;

    return true;
}

static libc_dl_module_t* LibC_dl_find_module_by_path(const char* path)
{
    for (libc_dl_module_t* module = LibC_dl_modules; module; module = module->next)
    {
        if (strcmp(module->path, path) == 0)
            return module;
    }

    return NULL;
}

static bool LibC_dl_symbol_name_valid(const libc_dl_module_t* module, uint32_t st_name)
{
    if (!module || !module->strtab || module->strtab_size == 0U)
        return false;
    if (st_name >= module->strtab_size)
        return false;

    const char* cursor = module->strtab + st_name;
    size_t remaining = module->strtab_size - st_name;
    for (size_t i = 0; i < remaining; i++)
    {
        if (cursor[i] == '\0')
            return true;
    }

    return false;
}

static bool LibC_dl_find_defined_symbol(libc_dl_module_t* module,
                                        const char* name,
                                        libc_elf64_sym_t** out_sym)
{
    if (!module || !name || !out_sym || !module->symtab || !module->strtab)
        return false;

    for (size_t i = 0; i < module->symbol_count; i++)
    {
        libc_elf64_sym_t* sym = &module->symtab[i];
        if (sym->st_name == 0U || sym->st_shndx == LIBC_ELF_SHN_UNDEF)
            continue;

        uint8_t bind = LIBC_ELF64_ST_BIND(sym->st_info);
        if (bind != LIBC_ELF_STB_GLOBAL && bind != LIBC_ELF_STB_WEAK)
            continue;
        if (!LibC_dl_symbol_name_valid(module, sym->st_name))
            continue;
        if (strcmp(module->strtab + sym->st_name, name) == 0)
        {
            *out_sym = sym;
            return true;
        }
    }

    return false;
}

static bool LibC_dl_lookup_symbol_global(libc_dl_module_t* requester,
                                         const char* name,
                                         libc_dl_module_t** out_owner,
                                         libc_elf64_sym_t** out_sym)
{
    if (!name || !out_owner || !out_sym)
        return false;

    if (requester)
    {
        libc_elf64_sym_t* local_sym = NULL;
        if (LibC_dl_find_defined_symbol(requester, name, &local_sym))
        {
            *out_owner = requester;
            *out_sym = local_sym;
            return true;
        }
    }

    for (libc_dl_module_t* module = LibC_dl_modules; module; module = module->next)
    {
        if (module == requester)
            continue;

        libc_elf64_sym_t* sym = NULL;
        if (LibC_dl_find_defined_symbol(module, name, &sym))
        {
            *out_owner = module;
            *out_sym = sym;
            return true;
        }
    }

    return false;
}

static bool LibC_dl_call_init(libc_dl_module_t* module)
{
    if (!module || module->initialized)
        return true;

    if (module->init_func != 0U)
    {
        void (*init_fn)(void) = (void (*)(void)) module->init_func;
        init_fn();
    }

    if (module->init_array != 0U && module->init_array_count != 0U)
    {
        uintptr_t* init_entries = (uintptr_t*) module->init_array;
        for (size_t i = 0; i < module->init_array_count; i++)
        {
            if (init_entries[i] == 0U)
                continue;
            void (*init_fn)(void) = (void (*)(void)) init_entries[i];
            init_fn();
        }
    }

    module->initialized = true;
    return true;
}

static void LibC_dl_call_fini(libc_dl_module_t* module)
{
    if (!module || !module->initialized)
        return;

    if (module->fini_array != 0U && module->fini_array_count != 0U)
    {
        uintptr_t* fini_entries = (uintptr_t*) module->fini_array;
        for (size_t i = module->fini_array_count; i > 0U; i--)
        {
            uintptr_t fn_addr = fini_entries[i - 1U];
            if (fn_addr == 0U)
                continue;
            void (*fini_fn)(void) = (void (*)(void)) fn_addr;
            fini_fn();
        }
    }

    if (module->fini_func != 0U)
    {
        void (*fini_fn)(void) = (void (*)(void)) module->fini_func;
        fini_fn();
    }

    module->initialized = false;
}

static bool LibC_dl_apply_relocations(libc_dl_module_t* module,
                                      libc_elf64_rela_t* relocs,
                                      size_t reloc_count)
{
    if (!module)
        return false;
    if (reloc_count == 0U)
        return true;
    if (!relocs)
    {
        LibC_dl_set_error("missing relocation table in '%s'", module->path);
        return false;
    }

    for (size_t i = 0; i < reloc_count; i++)
    {
        const libc_elf64_rela_t* rela = &relocs[i];
        uint32_t type = LIBC_ELF64_R_TYPE(rela->r_info);
        uint32_t sym_index = LIBC_ELF64_R_SYM(rela->r_info);

        uintptr_t reloc_addr = module->load_bias + (uintptr_t) rela->r_offset;
        if (!LibC_dl_addr_range_valid(module, reloc_addr, sizeof(uint64_t)))
        {
            LibC_dl_set_error("relocation address out of range in '%s'", module->path);
            return false;
        }

        uint64_t value = 0U;
        libc_elf64_sym_t* sym = NULL;
        libc_dl_module_t* owner = NULL;

        if (type != LIBC_ELF_R_X86_64_RELATIVE && type != LIBC_ELF_R_X86_64_NONE)
        {
            if (!module->symtab || sym_index >= module->symbol_count)
            {
                LibC_dl_set_error("symbol index out of range in '%s'", module->path);
                return false;
            }

            if (sym_index == LIBC_ELF_STN_UNDEF)
            {
                sym = NULL;
                owner = NULL;
            }
            else
            {
            sym = &module->symtab[sym_index];
            if (sym->st_shndx != LIBC_ELF_SHN_UNDEF)
            {
                owner = module;
            }
            else
            {
                if (sym->st_name == 0U)
                {
                    owner = NULL;
                    sym = NULL;
                    goto relocation_symbol_done;
                }

                if (!LibC_dl_symbol_name_valid(module, sym->st_name))
                {
                    LibC_dl_set_error("invalid undefined symbol in '%s'", module->path);
                    return false;
                }

                const char* name = module->strtab + sym->st_name;
                libc_elf64_sym_t* resolved = NULL;
                if (!LibC_dl_lookup_symbol_global(module, name, &owner, &resolved))
                {
                    uint8_t bind = LIBC_ELF64_ST_BIND(sym->st_info);
                    if (bind == LIBC_ELF_STB_WEAK)
                    {
                        owner = NULL;
                        resolved = NULL;
                    }
                    else
                    {
                        LibC_dl_set_error("unresolved symbol '%s' in '%s'", name, module->path);
                        return false;
                    }
                }
                sym = resolved;
            }
            }
        }
relocation_symbol_done:

        switch (type)
        {
            case LIBC_ELF_R_X86_64_NONE:
                break;

            case LIBC_ELF_R_X86_64_RELATIVE:
            {
                intptr_t rel_value = (intptr_t) module->load_bias + (intptr_t) rela->r_addend;
                value = (uint64_t) rel_value;
                *(uint64_t*) reloc_addr = value;
                break;
            }

            case LIBC_ELF_R_X86_64_64:
            case LIBC_ELF_R_X86_64_GLOB_DAT:
            case LIBC_ELF_R_X86_64_JUMP_SLOT:
                if (sym && owner)
                    value = owner->load_bias + sym->st_value;
                else
                    value = 0U;
                value = (uint64_t) ((intptr_t) value + (intptr_t) rela->r_addend);
                *(uint64_t*) reloc_addr = value;
                break;

            case LIBC_ELF_R_X86_64_DTPMOD64:
            {
                size_t module_id = module->tls_module_id;
                if (sym && owner)
                    module_id = owner->tls_module_id;
                *(uint64_t*) reloc_addr = (uint64_t) module_id;
                break;
            }

            case LIBC_ELF_R_X86_64_DTPOFF64:
            {
                uint64_t off = 0U;
                if (sym)
                    off = sym->st_value;
                off = (uint64_t) ((intptr_t) off + (intptr_t) rela->r_addend);
                *(uint64_t*) reloc_addr = off;
                break;
            }

            default:
                LibC_dl_set_error("unsupported relocation type %u in '%s'", type, module->path);
                return false;
        }
    }

    return true;
}

static bool LibC_dl_apply_protections(libc_dl_module_t* module)
{
    if (!module)
        return false;

    for (size_t i = 0; i < module->segment_count; i++)
    {
        libc_dl_segment_t* segment = &module->segments[i];
        if (segment->size == 0U)
            continue;
        if (mprotect((void*) segment->addr, segment->size, segment->prot) < 0)
        {
            LibC_dl_set_error("mprotect failed for '%s'", module->path);
            return false;
        }
    }

    return true;
}

static bool LibC_dl_parse_dynamic(libc_dl_module_t* module)
{
    if (!module || !module->dynamic || module->dynamic_count == 0U)
        return false;

    uintptr_t hash_addr = 0U;
    uintptr_t rela_addr = 0U;
    uintptr_t rela_size = 0U;
    uintptr_t rela_ent = sizeof(libc_elf64_rela_t);
    uintptr_t jmprel_addr = 0U;
    uintptr_t jmprel_size = 0U;
    uintptr_t plt_rel_type = LIBC_ELF_DT_RELA;
    uintptr_t strtab_addr = 0U;
    uintptr_t strtab_size = 0U;
    uintptr_t symtab_addr = 0U;
    uintptr_t sym_ent = sizeof(libc_elf64_sym_t);

    module->needed_count = 0U;
    module->needed_module_count = 0U;
    module->init_func = 0U;
    module->fini_func = 0U;
    module->init_array = 0U;
    module->init_array_count = 0U;
    module->fini_array = 0U;
    module->fini_array_count = 0U;

    for (size_t i = 0; i < module->dynamic_count; i++)
    {
        const libc_elf64_dyn_t* dyn = &module->dynamic[i];
        if (dyn->d_tag == LIBC_ELF_DT_NULL)
            break;

        switch (dyn->d_tag)
        {
            case LIBC_ELF_DT_NEEDED:
                if (module->needed_count < LIBC_DL_MAX_NEEDED)
                    module->needed_offsets[module->needed_count++] = (uint32_t) dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_HASH:
                hash_addr = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_STRTAB:
                strtab_addr = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_STRSZ:
                strtab_size = dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_SYMTAB:
                symtab_addr = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_SYMENT:
                sym_ent = dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_RELA:
                rela_addr = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_RELASZ:
                rela_size = dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_RELAENT:
                rela_ent = dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_PLTREL:
                plt_rel_type = dyn->d_un.d_val;
                break;
            case LIBC_ELF_DT_JMPREL:
                jmprel_addr = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_INIT:
                module->init_func = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_FINI:
                module->fini_func = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_INIT_ARRAY:
                module->init_array = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_INIT_ARRAYSZ:
                module->init_array_count = (size_t) (dyn->d_un.d_val / sizeof(uintptr_t));
                break;
            case LIBC_ELF_DT_FINI_ARRAY:
                module->fini_array = module->load_bias + dyn->d_un.d_ptr;
                break;
            case LIBC_ELF_DT_FINI_ARRAYSZ:
                module->fini_array_count = (size_t) (dyn->d_un.d_val / sizeof(uintptr_t));
                break;
            case LIBC_ELF_DT_PLTRELSZ:
                jmprel_size = dyn->d_un.d_val;
                break;
            default:
                break;
        }
    }

    if (strtab_addr == 0U || symtab_addr == 0U || hash_addr == 0U)
    {
        LibC_dl_set_error("missing dynamic tables in '%s' (requires DT_HASH)", module->path);
        return false;
    }
    if (strtab_size == 0U)
    {
        LibC_dl_set_error("invalid string table size in '%s'", module->path);
        return false;
    }
    if (sym_ent != sizeof(libc_elf64_sym_t))
    {
        LibC_dl_set_error("unsupported symbol entry size in '%s'", module->path);
        return false;
    }
    if (rela_ent == 0U)
        rela_ent = sizeof(libc_elf64_rela_t);
    if (rela_ent != sizeof(libc_elf64_rela_t))
    {
        LibC_dl_set_error("unsupported relocation entry size in '%s'", module->path);
        return false;
    }
    if (plt_rel_type != LIBC_ELF_DT_RELA)
    {
        LibC_dl_set_error("unsupported PLT relocation format in '%s'", module->path);
        return false;
    }

    if (!LibC_dl_addr_range_valid(module, hash_addr, sizeof(uint32_t) * 2U))
    {
        LibC_dl_set_error("invalid DT_HASH pointer in '%s'", module->path);
        return false;
    }

    const uint32_t* hash = (const uint32_t*) hash_addr;
    uint32_t nchain = hash[1];

    if (!LibC_dl_addr_range_valid(module, strtab_addr, (size_t) strtab_size))
    {
        LibC_dl_set_error("invalid DT_STRTAB range in '%s'", module->path);
        return false;
    }
    if (!LibC_dl_addr_range_valid(module, symtab_addr, (size_t) nchain * sizeof(libc_elf64_sym_t)))
    {
        LibC_dl_set_error("invalid DT_SYMTAB range in '%s'", module->path);
        return false;
    }

    module->strtab = (const char*) strtab_addr;
    module->strtab_size = (size_t) strtab_size;
    module->symtab = (libc_elf64_sym_t*) symtab_addr;
    module->symbol_count = (size_t) nchain;

    if (rela_addr != 0U && rela_size != 0U)
    {
        if ((rela_size % sizeof(libc_elf64_rela_t)) != 0U ||
            !LibC_dl_addr_range_valid(module, rela_addr, (size_t) rela_size))
        {
            LibC_dl_set_error("invalid DT_RELA range in '%s'", module->path);
            return false;
        }
        module->rela = (libc_elf64_rela_t*) rela_addr;
        module->rela_count = (size_t) (rela_size / sizeof(libc_elf64_rela_t));
    }
    else
    {
        module->rela = NULL;
        module->rela_count = 0U;
    }

    if (jmprel_addr != 0U && jmprel_size != 0U)
    {
        if ((jmprel_size % sizeof(libc_elf64_rela_t)) != 0U ||
            !LibC_dl_addr_range_valid(module, jmprel_addr, (size_t) jmprel_size))
        {
            LibC_dl_set_error("invalid DT_JMPREL range in '%s'", module->path);
            return false;
        }
        module->plt_rela = (libc_elf64_rela_t*) jmprel_addr;
        module->plt_rela_count = (size_t) (jmprel_size / sizeof(libc_elf64_rela_t));
    }
    else
    {
        module->plt_rela = NULL;
        module->plt_rela_count = 0U;
    }

    return true;
}

static bool LibC_dl_load_elf_image(libc_dl_module_t* module,
                                   const uint8_t* image,
                                   size_t image_size)
{
    if (!module || !image || image_size < sizeof(libc_elf64_ehdr_t))
        return false;

    const libc_elf64_ehdr_t* ehdr = (const libc_elf64_ehdr_t*) image;
    if (ehdr->e_ident[0] != 0x7FU ||
        ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' ||
        ehdr->e_ident[3] != 'F' ||
        ehdr->e_ident[4] != 2U ||
        ehdr->e_ident[5] != 1U ||
        ehdr->e_type != LIBC_ELF_ET_DYN ||
        ehdr->e_machine != LIBC_ELF_EM_X86_64)
    {
        LibC_dl_set_error("unsupported ELF format in '%s'", module->path);
        return false;
    }

    if (ehdr->e_phnum == 0U ||
        ehdr->e_phentsize != sizeof(libc_elf64_phdr_t) ||
        ehdr->e_phoff > image_size)
    {
        LibC_dl_set_error("invalid ELF program header table in '%s'", module->path);
        return false;
    }

    uint64_t ph_size = (uint64_t) ehdr->e_phnum * sizeof(libc_elf64_phdr_t);
    if (ph_size > (uint64_t) image_size - ehdr->e_phoff)
    {
        LibC_dl_set_error("program header table out of bounds in '%s'", module->path);
        return false;
    }

    const libc_elf64_phdr_t* phdrs = (const libc_elf64_phdr_t*) (image + ehdr->e_phoff);

    uintptr_t min_vaddr = UINTPTR_MAX;
    uintptr_t max_vaddr = 0U;
    bool has_load = false;
    module->segment_count = 0U;
    module->dynamic = NULL;
    module->dynamic_count = 0U;
    module->tls_vaddr = 0U;
    module->tls_filesz = 0U;
    module->tls_memsz = 0U;
    module->tls_align = 0U;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const libc_elf64_phdr_t* phdr = &phdrs[i];
        if (phdr->p_type != LIBC_ELF_PT_LOAD)
            continue;
        if (phdr->p_memsz == 0U)
            continue;
        if (phdr->p_filesz > phdr->p_memsz)
        {
            LibC_dl_set_error("invalid PT_LOAD sizes in '%s'", module->path);
            return false;
        }
        if (phdr->p_offset > image_size || phdr->p_filesz > image_size - phdr->p_offset)
        {
            LibC_dl_set_error("PT_LOAD out of file bounds in '%s'", module->path);
            return false;
        }

        uintptr_t seg_start = LibC_dl_align_down_page((uintptr_t) phdr->p_vaddr);
        uintptr_t seg_end_raw = (uintptr_t) phdr->p_vaddr + (uintptr_t) phdr->p_memsz;
        if (seg_end_raw < (uintptr_t) phdr->p_vaddr)
        {
            LibC_dl_set_error("PT_LOAD overflow in '%s'", module->path);
            return false;
        }

        uintptr_t seg_end = LibC_dl_align_up_page(seg_end_raw);
        if (seg_end == 0U || seg_end < seg_start)
        {
            LibC_dl_set_error("PT_LOAD alignment overflow in '%s'", module->path);
            return false;
        }

        if (seg_start < min_vaddr)
            min_vaddr = seg_start;
        if (seg_end > max_vaddr)
            max_vaddr = seg_end;
        has_load = true;
    }

    if (!has_load || min_vaddr == UINTPTR_MAX || max_vaddr <= min_vaddr)
    {
        LibC_dl_set_error("ELF has no loadable segment in '%s'", module->path);
        return false;
    }

    size_t map_size = (size_t) (max_vaddr - min_vaddr);
    void* map_base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base == MAP_FAILED)
    {
        LibC_dl_set_error("mmap failed for '%s'", module->path);
        return false;
    }

    module->map_base = map_base;
    module->map_size = map_size;
    module->load_bias = (uintptr_t) map_base - min_vaddr;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        const libc_elf64_phdr_t* phdr = &phdrs[i];

        if (phdr->p_type == LIBC_ELF_PT_LOAD && phdr->p_memsz != 0U)
        {
            uintptr_t dst = module->load_bias + (uintptr_t) phdr->p_vaddr;
            if (!LibC_dl_addr_range_valid(module, dst, (size_t) phdr->p_memsz))
            {
                LibC_dl_set_error("PT_LOAD mapped out of bounds in '%s'", module->path);
                return false;
            }

            if (phdr->p_filesz != 0U)
                memcpy((void*) dst, image + phdr->p_offset, (size_t) phdr->p_filesz);
            if (phdr->p_memsz > phdr->p_filesz)
                memset((void*) (dst + (uintptr_t) phdr->p_filesz), 0, (size_t) (phdr->p_memsz - phdr->p_filesz));

            uintptr_t seg_page_start = module->load_bias + LibC_dl_align_down_page((uintptr_t) phdr->p_vaddr);
            uintptr_t seg_page_end = module->load_bias + LibC_dl_align_up_page((uintptr_t) phdr->p_vaddr + (uintptr_t) phdr->p_memsz);
            if (seg_page_end <= seg_page_start)
            {
                LibC_dl_set_error("invalid PT_LOAD page range in '%s'", module->path);
                return false;
            }

            int final_prot = PROT_READ;
            if ((phdr->p_flags & LIBC_ELF_PF_W) != 0U)
                final_prot |= PROT_WRITE;
            if ((phdr->p_flags & LIBC_ELF_PF_X) != 0U)
                final_prot |= PROT_EXEC;
            if ((final_prot & PROT_WRITE) != 0 && (final_prot & PROT_EXEC) != 0)
                final_prot = PROT_READ | PROT_EXEC;

            if (module->segment_count >= LIBC_DL_MAX_SEGMENTS)
            {
                LibC_dl_set_error("too many PT_LOAD segments in '%s'", module->path);
                return false;
            }

            module->segments[module->segment_count].addr = seg_page_start;
            module->segments[module->segment_count].size = (size_t) (seg_page_end - seg_page_start);
            module->segments[module->segment_count].prot = final_prot;
            module->segment_count++;
        }
        else if (phdr->p_type == LIBC_ELF_PT_DYNAMIC)
        {
            uintptr_t dyn_addr = module->load_bias + (uintptr_t) phdr->p_vaddr;
            if (phdr->p_memsz < sizeof(libc_elf64_dyn_t) ||
                !LibC_dl_addr_range_valid(module, dyn_addr, (size_t) phdr->p_memsz))
            {
                LibC_dl_set_error("invalid PT_DYNAMIC in '%s'", module->path);
                return false;
            }

            module->dynamic = (libc_elf64_dyn_t*) dyn_addr;
            module->dynamic_count = (size_t) (phdr->p_memsz / sizeof(libc_elf64_dyn_t));
        }
        else if (phdr->p_type == LIBC_ELF_PT_TLS)
        {
            module->tls_vaddr = phdr->p_vaddr;
            module->tls_filesz = (size_t) phdr->p_filesz;
            module->tls_memsz = (size_t) phdr->p_memsz;
            module->tls_align = (size_t) phdr->p_align;
        }
    }

    if (!module->dynamic || module->dynamic_count == 0U)
    {
        LibC_dl_set_error("missing PT_DYNAMIC in '%s'", module->path);
        return false;
    }

    return LibC_dl_parse_dynamic(module);
}

static bool LibC_dl_register_tls(libc_dl_module_t* module)
{
    if (!module)
        return false;
    if (module->tls_memsz == 0U)
        return true;

    if (module->tls_filesz > module->tls_memsz)
    {
        LibC_dl_set_error("invalid PT_TLS range in '%s'", module->path);
        return false;
    }

    const void* init_image = NULL;
    if (module->tls_filesz != 0U)
    {
        init_image = (const void*) (module->load_bias + module->tls_vaddr);
        if (!LibC_dl_addr_range_valid(module, (uintptr_t) init_image, module->tls_filesz))
        {
            LibC_dl_set_error("invalid PT_TLS range in '%s'", module->path);
            return false;
        }
    }

    size_t module_id = 0U;
    if (__libc_tls_module_register(init_image,
                                   module->tls_filesz,
                                   module->tls_memsz,
                                   module->tls_align,
                                   &module_id) < 0)
    {
        LibC_dl_set_error("TLS module registration failed for '%s'", module->path);
        return false;
    }

    module->tls_module_id = module_id;
    return true;
}

static void LibC_dl_unregister_tls(libc_dl_module_t* module)
{
    if (!module || module->tls_module_id == 0U)
        return;
    (void) __libc_tls_module_unregister(module->tls_module_id);
    module->tls_module_id = 0U;
}

static bool LibC_dl_insert_module(libc_dl_module_t* module)
{
    if (!module)
        return false;
    module->next = LibC_dl_modules;
    LibC_dl_modules = module;
    return true;
}

static void LibC_dl_remove_module(libc_dl_module_t* module)
{
    if (!module)
        return;

    libc_dl_module_t** it = &LibC_dl_modules;
    while (*it)
    {
        if (*it == module)
        {
            *it = module->next;
            module->next = NULL;
            return;
        }
        it = &(*it)->next;
    }
}

static bool LibC_dl_release_module(libc_dl_module_t* module)
{
    if (!module)
        return false;
    if (module->ref_count == 0U)
        return true;

    module->ref_count--;
    if (module->ref_count != 0U)
        return true;

    LibC_dl_call_fini(module);
    LibC_dl_remove_module(module);

    for (size_t i = 0; i < module->needed_module_count; i++)
    {
        libc_dl_module_t* dep = module->needed_modules[i];
        if (dep)
            (void) LibC_dl_release_module(dep);
    }

    LibC_dl_unregister_tls(module);
    if (module->map_base && module->map_size != 0U)
        (void) munmap(module->map_base, module->map_size);
    free(module);
    return true;
}

static bool LibC_dl_load_module_internal(const char* path,
                                         int mode,
                                         const char* parent_dir,
                                         libc_dl_module_t** out_module)
{
    if (!path || !out_module)
        return false;

    char resolved_path[LIBC_DL_PATH_MAX];
    if (!LibC_dl_resolve_path(path, parent_dir, resolved_path, sizeof(resolved_path)))
    {
        LibC_dl_set_error("cannot resolve shared object '%s'", path);
        return false;
    }

    libc_dl_module_t* existing = LibC_dl_find_module_by_path(resolved_path);
    if (existing)
    {
        if (existing->ref_count < UINT32_MAX)
            existing->ref_count++;
        if ((mode & RTLD_GLOBAL) != 0)
            existing->global_scope = true;
        *out_module = existing;
        return true;
    }

    libc_dl_module_t* module = (libc_dl_module_t*) calloc(1U, sizeof(*module));
    if (!module)
    {
        LibC_dl_set_error("out of memory while loading '%s'", resolved_path);
        return false;
    }

    if (!LibC_dl_copy_path(module->path, sizeof(module->path), resolved_path))
    {
        LibC_dl_set_error("path too long for '%s'", resolved_path);
        free(module);
        return false;
    }

    uint8_t* image = NULL;
    size_t image_size = 0U;
    if (!LibC_dl_read_file(resolved_path, &image, &image_size))
    {
        LibC_dl_set_error("unable to read '%s'", resolved_path);
        free(module);
        return false;
    }

    if (!LibC_dl_load_elf_image(module, image, image_size))
    {
        free(image);
        if (module->map_base && module->map_size != 0U)
            (void) munmap(module->map_base, module->map_size);
        free(module);
        return false;
    }
    free(image);

    if (!LibC_dl_register_tls(module))
    {
        if (module->map_base && module->map_size != 0U)
            (void) munmap(module->map_base, module->map_size);
        free(module);
        return false;
    }

    module->ref_count = 1U;
    module->global_scope = (mode & RTLD_GLOBAL) != 0;

    LibC_dl_insert_module(module);

    char module_parent_dir[LIBC_DL_PATH_MAX];
    bool module_has_parent = LibC_dl_parent_dir(module->path, module_parent_dir, sizeof(module_parent_dir));
    bool load_ok = true;
    for (size_t i = 0; i < module->needed_count; i++)
    {
        uint32_t str_off = module->needed_offsets[i];
        if (!LibC_dl_symbol_name_valid(module, str_off))
        {
            LibC_dl_set_error("invalid DT_NEEDED string index in '%s'", module->path);
            load_ok = false;
            break;
        }

        const char* dependency_name = module->strtab + str_off;
        if (dependency_name[0] == '\0')
            continue;
        if (module->needed_module_count >= LIBC_DL_MAX_NEEDED)
        {
            LibC_dl_set_error("too many dependencies in '%s'", module->path);
            load_ok = false;
            break;
        }

        libc_dl_module_t* dependency = NULL;
        if (!LibC_dl_load_module_internal(dependency_name,
                                          mode,
                                          module_has_parent ? module_parent_dir : NULL,
                                          &dependency))
        {
            load_ok = false;
            break;
        }

        module->needed_modules[module->needed_module_count++] = dependency;
    }

    if (!load_ok ||
        !LibC_dl_apply_relocations(module, module->rela, module->rela_count) ||
        !LibC_dl_apply_relocations(module, module->plt_rela, module->plt_rela_count) ||
        !LibC_dl_apply_protections(module) ||
        !LibC_dl_call_init(module))
    {
        LibC_dl_remove_module(module);
        for (size_t i = 0; i < module->needed_module_count; i++)
        {
            libc_dl_module_t* dependency = module->needed_modules[i];
            if (dependency)
                (void) LibC_dl_release_module(dependency);
        }
        LibC_dl_unregister_tls(module);
        if (module->map_base && module->map_size != 0U)
            (void) munmap(module->map_base, module->map_size);
        free(module);
        return false;
    }

    *out_module = module;
    return true;
}

static bool LibC_dl_handle_is_main(void* handle)
{
    return handle == (void*) &LibC_dl_main_program_sentinel;
}

static bool LibC_dl_symbol_lookup_from_handle(void* handle,
                                              const char* symbol,
                                              void** out_addr)
{
    if (!symbol || !out_addr)
        return false;

    *out_addr = NULL;

    if (handle == NULL || LibC_dl_handle_is_main(handle))
    {
        libc_dl_module_t* owner = NULL;
        libc_elf64_sym_t* sym = NULL;
        if (!LibC_dl_lookup_symbol_global(NULL, symbol, &owner, &sym))
            return false;
        if (!owner || !sym)
            return false;
        *out_addr = (void*) (owner->load_bias + sym->st_value);
        return true;
    }

    libc_dl_module_t* module = (libc_dl_module_t*) handle;
    bool belongs_to_loader = false;
    for (libc_dl_module_t* it = LibC_dl_modules; it; it = it->next)
    {
        if (it == module)
        {
            belongs_to_loader = true;
            break;
        }
    }
    if (!belongs_to_loader)
        return false;

    libc_elf64_sym_t* local = NULL;
    if (LibC_dl_find_defined_symbol(module, symbol, &local))
    {
        *out_addr = (void*) (module->load_bias + local->st_value);
        return true;
    }

    libc_dl_module_t* owner = NULL;
    libc_elf64_sym_t* sym = NULL;
    if (!LibC_dl_lookup_symbol_global(module, symbol, &owner, &sym))
        return false;
    if (!owner || !sym)
        return false;
    *out_addr = (void*) (owner->load_bias + sym->st_value);
    return true;
}

void* dlopen(const char* path, int mode)
{
    LibC_dl_clear_error();

    if ((mode & (RTLD_LAZY | RTLD_NOW)) == 0)
        mode |= RTLD_NOW;

    if (path == NULL)
        return (void*) &LibC_dl_main_program_sentinel;

    LibC_dl_lock();
    libc_dl_module_t* module = NULL;
    bool ok = LibC_dl_load_module_internal(path, mode, NULL, &module);
    LibC_dl_unlock();

    if (!ok)
    {
        if (!LibC_dl_error_ready)
            LibC_dl_set_error("dlopen failed for '%s'", path);
        return NULL;
    }

    return (void*) module;
}

int dlclose(void* handle)
{
    LibC_dl_clear_error();

    if (!handle || LibC_dl_handle_is_main(handle))
        return 0;

    LibC_dl_lock();
    libc_dl_module_t* module = (libc_dl_module_t*) handle;
    bool belongs_to_loader = false;
    for (libc_dl_module_t* it = LibC_dl_modules; it; it = it->next)
    {
        if (it == module)
        {
            belongs_to_loader = true;
            break;
        }
    }
    if (!belongs_to_loader)
    {
        LibC_dl_unlock();
        LibC_dl_set_error("invalid dlclose handle");
        errno = EINVAL;
        return -1;
    }

    bool ok = LibC_dl_release_module(module);
    LibC_dl_unlock();
    if (!ok)
    {
        errno = EINVAL;
        if (!LibC_dl_error_ready)
            LibC_dl_set_error("dlclose failed");
        return -1;
    }

    return 0;
}

void* dlsym(void* handle, const char* symbol)
{
    LibC_dl_clear_error();

    if (!symbol || symbol[0] == '\0')
    {
        errno = EINVAL;
        LibC_dl_set_error("invalid symbol name");
        return NULL;
    }

    if (strlen(symbol) >= LIBC_DL_MAX_SYMBOL_LOOKUP)
    {
        errno = ENAMETOOLONG;
        LibC_dl_set_error("symbol name too long");
        return NULL;
    }

    LibC_dl_lock();
    void* symbol_addr = NULL;
    bool ok = LibC_dl_symbol_lookup_from_handle(handle, symbol, &symbol_addr);
    LibC_dl_unlock();

    if (!ok || symbol_addr == NULL)
    {
        errno = ENOENT;
        LibC_dl_set_error("symbol '%s' not found", symbol);
        return NULL;
    }

    return symbol_addr;
}

char* dlerror(void)
{
    if (!LibC_dl_error_ready)
        return NULL;

    LibC_dl_error_ready = false;
    return LibC_dl_error_message;
}
