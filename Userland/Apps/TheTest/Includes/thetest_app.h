#ifndef THETEST_APP_H
#define THETEST_APP_H

#include <stdbool.h>

typedef struct thetest_options
{
    bool wm_mode;
    bool help;
} thetest_options_t;

bool thetest_parse_options(int argc, char** argv, thetest_options_t* out_opts);
void thetest_print_help(const char* prog);

int thetest_run_cli(int argc, char** argv, char** envp);
int thetest_run_wm(const char* self_path);

#endif
