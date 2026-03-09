#include <stdio.h>
#include <limits.h>
#include <syscall.h>
#include <unistd.h>

#define OPTIONAL_ARGS \
    OPTIONAL_UINT_ARG(sleep_state, UINT_MAX, "--sleep", "state", "Enter ACPI sleep state (0..5)")

#define BOOLEAN_ARGS \
    BOOLEAN_ARG(shutdown, "-s", "Shutdown (ACPI S5 via SYS_POWER)") \
    BOOLEAN_ARG(reboot, "-r", "Reboot (ACPI reset via SYS_POWER)") \
    BOOLEAN_ARG(help, "-h", "Show help") \
    BOOLEAN_ARG(help_long, "--help", "Show help")

#include <easyargs.h>

static void powermanager_print_help(const char* prog)
{
    const char* name = (prog && prog[0] != '\0') ? prog : "ThePowerManager";
    printf("Usage: %s [option]\n", name);
    printf("  -s             Shutdown (ACPI S5)\n");
    printf("  -r             Reboot (ACPI reset)\n");
    printf("  --sleep <n>    Enter ACPI S-state n (0..5)\n");
    printf("                 S0: running, S1/S2/S3: sleep, S4: hibernate, S5: soft-off\n");
    printf("  -h, --help     Show this help\n");
}

static int powermanager_handle_sleep(unsigned int state)
{
    if (state > SYS_SLEEP_STATE_S5)
    {
        printf("[ThePowerManager] invalid sleep state %u (expected 0..5)\n", state);
        return 1;
    }

    if (state == SYS_SLEEP_STATE_S0)
    {
        printf("[ThePowerManager] S0 requested: system is already in running state\n");
        return 0;
    }

    if (state == SYS_SLEEP_STATE_S5)
    {
        printf("[ThePowerManager] S5 requested (shutdown)\n");
        if (shutdown() == 0)
            return 0;
        printf("[ThePowerManager] shutdown failed\n");
        return 1;
    }

    printf("[ThePowerManager] sleep S%u requested\n", state);
    if (sleep_state(state) == 0)
        return 0;
    printf("[ThePowerManager] sleep S%u failed\n", state);
    return 1;
}

int main(int argc, char** argv)
{
    args_t args = make_default_args();
    if (!parse_args(argc, argv, &args))
    {
        powermanager_print_help((argc > 0) ? argv[0] : "ThePowerManager");
        return 1;
    }

    if (args.help || args.help_long)
    {
        powermanager_print_help((argc > 0) ? argv[0] : "ThePowerManager");
        return 0;
    }

    if (args.shutdown && args.reboot)
    {
        printf("[ThePowerManager] choose only one action: -s, -r, or --sleep <state>\n");
        return 1;
    }
    if (args.sleep_state != UINT_MAX && (args.shutdown || args.reboot))
    {
        printf("[ThePowerManager] choose only one action: -s, -r, or --sleep <state>\n");
        return 1;
    }

    if (args.shutdown)
    {
        printf("[ThePowerManager] shutdown requested\n");
        if (shutdown() == 0)
            return 0;
        printf("[ThePowerManager] shutdown failed\n");
        return 1;
    }

    if (args.reboot)
    {
        printf("[ThePowerManager] reboot requested\n");
        if (reboot() == 0)
            return 0;
        printf("[ThePowerManager] reboot failed\n");
        return 1;
    }

    if (args.sleep_state != UINT_MAX)
    {
        return powermanager_handle_sleep(args.sleep_state);
    }

    if (argc <= 1)
    {
        powermanager_print_help((argc > 0) ? argv[0] : "ThePowerManager");
        return 0;
    }
    else
    {
        printf("[ThePowerManager] no valid action selected\n");
        powermanager_print_help((argc > 0) ? argv[0] : "ThePowerManager");
        return 1;
    }
}
