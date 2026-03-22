#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/objlist.h"
#include "py/mperrno.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "shared/runtime/pyexec.h"

static char *stack_top;
#define THEOS_MPY_PATH_MAX     256U
#define THEOS_MPY_FILE_MAX     (512U * 1024U)

#if MICROPY_ENABLE_GC
static char heap[MICROPY_HEAP_SIZE];
#endif

static bool theos_mpy_abspath(const char* path, char* out, size_t out_size)
{
    if (!path || !out || out_size < 2U || path[0] == '\0')
        return false;

    if (path[0] == '/')
    {
        size_t len = strlen(path);
        if (len + 1U > out_size)
            return false;
        memcpy(out, path, len + 1U);
        return true;
    }

    if (path[0] == '.' && path[1] == '/')
        path += 2;

    size_t len = strlen(path);
    if (len + 2U > out_size)
        return false;

    out[0] = '/';
    memcpy(out + 1U, path, len + 1U);
    return true;
}

static void theos_mpy_print_usage(const char* argv0)
{
    const char* prog = (argv0 && argv0[0] != '\0') ? argv0 : "MicroPython";
    printf("usage: %s [--help] [script.py]\n", prog);
}

static int theos_mpy_convert_pyexec_result(int ret)
{
#if MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING
    if (ret & PYEXEC_FORCED_EXIT)
        return ret & 0xFF;
    return ret;
#else
    if (ret == PYEXEC_NORMAL_EXIT)
        return 0;
    if (ret & PYEXEC_FORCED_EXIT)
        return ret & 0xFF;
    return 1;
#endif
}

#if MICROPY_PY_SYS_ARGV
static void theos_mpy_set_sys_argv(int argc, char** argv)
{
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);
    for (int i = 0; i < argc; i++)
    {
        if (!argv[i])
            continue;
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(qstr_from_str(argv[i])));
    }
}
#endif

#if MICROPY_PY_SYS_EXIT
static void theos_mpy_install_exit_aliases(void)
{
    mp_obj_t exit_obj = MP_OBJ_FROM_PTR(&mp_sys_exit_obj);
    mp_store_global(qstr_from_str("exit"), exit_obj);
    mp_store_global(qstr_from_str("quit"), exit_obj);
}
#endif

int main(int argc, char **argv, char **envp)
{
    (void) envp;

    int stack_dummy;
    stack_top = (char *) &stack_dummy;

#if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
#endif

    mp_init();

#if MICROPY_PY_SYS_ARGV
    theos_mpy_set_sys_argv(argc, argv);
#endif

#if MICROPY_PY_SYS_EXIT
    theos_mpy_install_exit_aliases();
#endif

    if (argc > 1)
    {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        {
            theos_mpy_print_usage(argv[0]);
            mp_deinit();
            return 0;
        }

        char script_path[THEOS_MPY_PATH_MAX];
        if (!theos_mpy_abspath(argv[1], script_path, sizeof(script_path)))
        {
            printf("MicroPython: invalid script path '%s'\n", argv[1]);
            mp_deinit();
            return 2;
        }

        int rc = pyexec_file(script_path);
        mp_deinit();
        return theos_mpy_convert_pyexec_result(rc);
    }

    printf("[MicroPython] REPL ready\n");
    (void) pyexec_friendly_repl();
    mp_deinit();

    return 0;
}

#if MICROPY_ENABLE_GC
void gc_collect(void)
{
    // Conservative stack scan for roots.
    void *dummy;
    gc_collect_start();
    gc_collect_root(&dummy,
                    ((mp_uint_t) stack_top - (mp_uint_t) &dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}
#endif

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    const char* raw_path = qstr_str(filename);
    char abs_path[THEOS_MPY_PATH_MAX];
    if (!theos_mpy_abspath(raw_path, abs_path, sizeof(abs_path)))
        mp_raise_OSError(MP_ENOENT);

    int fd = sys_open(abs_path, SYS_OPEN_READ);
    if (fd < 0)
        mp_raise_OSError(MP_ENOENT);

    int64_t file_size64 = sys_lseek(fd, 0, SYS_SEEK_END);
    if (file_size64 >= 0)
        (void) sys_lseek(fd, 0, SYS_SEEK_SET);
    if (file_size64 < 0)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOENT);
    }
    if ((uint64_t) file_size64 > (uint64_t) THEOS_MPY_FILE_MAX)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOMEM);
    }

    size_t file_size_cap = (size_t) file_size64;
    size_t alloc_len = file_size_cap + 1U;
    if (alloc_len == 0U)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOMEM);
    }

    byte* file_buf = m_new_maybe(byte, alloc_len);
    if (!file_buf)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOMEM);
    }

    size_t file_size = 0;
    while (file_size < file_size_cap)
    {
        int rc = sys_read(fd, file_buf + file_size, file_size_cap - file_size);
        if (rc < 0)
        {
            (void) sys_close(fd);
            m_del(byte, file_buf, alloc_len);
            mp_raise_OSError(MP_ENOENT);
        }
        if (rc == 0)
            break;
        file_size += (size_t) rc;
    }
    (void) sys_close(fd);

    file_buf[file_size] = '\0';
    return mp_lexer_new_from_str_len(qstr_from_str(abs_path),
                                     (const char*) file_buf,
                                     file_size,
                                     alloc_len);
}

mp_import_stat_t mp_import_stat(const char *path)
{
    char abs_path[THEOS_MPY_PATH_MAX];
    if (!theos_mpy_abspath(path, abs_path, sizeof(abs_path)))
        return MP_IMPORT_STAT_NO_EXIST;

    if (fs_is_dir(abs_path) == 1)
        return MP_IMPORT_STAT_DIR;

    int fd = sys_open(abs_path, SYS_OPEN_READ);
    if (fd >= 0)
    {
        (void) sys_close(fd);
        return MP_IMPORT_STAT_FILE;
    }

    return MP_IMPORT_STAT_NO_EXIST;
}

void nlr_jump_fail(void *val)
{
    (void) val;
    for (;;)
        ;
}

void MP_NORETURN __fatal_error(const char *msg)
{
    if (msg)
        printf("MicroPython fatal: %s\n", msg);
    else
        printf("MicroPython fatal\n");

    for (;;)
        ;
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr)
{
    printf("Assertion '%s' failed at %s:%d (%s)\n", expr, file, line, func);
    __fatal_error("assert");
}
#endif
