#include "Includes/thetest_app.h"

#include <stdio.h>
#include <string.h>

#define BOOLEAN_ARGS                                                                 \
    BOOLEAN_ARG(wm, "--wm", "Run in WindowServer window mode")                      \
    BOOLEAN_ARG(no_wm, "--no-wm", "Force terminal mode (internal)")                 \
    BOOLEAN_ARG(help, "-h", "Show help")                                            \
    BOOLEAN_ARG(help_long, "--help", "Show help")

#include <easyargs.h>

bool thetest_parse_options(int argc, char** argv, thetest_options_t* out_opts)
{
    if (!out_opts)
        return false;

    args_t args = make_default_args();
    if (!parse_args(argc, argv, &args))
        return false;

    if (args.wm && args.no_wm)
        return false;

    out_opts->wm_mode = args.wm && !args.no_wm;
    out_opts->help = args.help || args.help_long;
    return true;
}

void thetest_print_help(const char* prog)
{
    const char* name = (prog && prog[0] != '\0') ? prog : "TheTest";
    printf("Usage: %s [options]\n", name);
    printf("  --wm            Run TheTest in a WindowServer window\n");
    printf("  --no-wm         Force terminal mode\n");
    printf("  -h, --help      Show help\n");
}
