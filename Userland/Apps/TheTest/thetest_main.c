#include "Includes/thetest_app.h"

int main(int argc, char** argv, char** envp)
{
    thetest_options_t opts = { 0 };
    if (!thetest_parse_options(argc, argv, &opts))
    {
        thetest_print_help((argc > 0) ? argv[0] : "TheTest");
        return 1;
    }

    if (opts.help)
    {
        thetest_print_help((argc > 0) ? argv[0] : "TheTest");
        return 0;
    }

    if (opts.wm_mode)
        return thetest_run_wm((argc > 0) ? argv[0] : "/bin/TheTest");

    return thetest_run_cli(argc, argv, envp);
}
