#ifndef _THESHELL_CORE_H
#define _THESHELL_CORE_H

#include <stdbool.h>
#include <stddef.h>

#define THESHELL_PATH_MAX 256U

typedef void (*theshell_write_fn)(void* opaque, const char* text, size_t len);
typedef void (*theshell_clear_fn)(void* opaque);

typedef struct theshell_io
{
    theshell_write_fn write;
    theshell_clear_fn clear;
    void* opaque;
} theshell_io_t;

typedef struct theshell_core
{
    char cwd[THESHELL_PATH_MAX];
    theshell_io_t io;
} theshell_core_t;

void theshell_core_init(theshell_core_t* core, const theshell_io_t* io);
const char* theshell_core_cwd(const theshell_core_t* core);
int theshell_core_execute_line(theshell_core_t* core, const char* line, bool* out_should_exit);
int theshell_core_autocomplete(theshell_core_t* core,
                               char* inout_line,
                               size_t line_size,
                               char out_matches[][THESHELL_PATH_MAX],
                               bool out_match_is_dir[],
                               size_t max_matches,
                               size_t* out_match_count);

#endif
