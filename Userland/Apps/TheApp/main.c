#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define THEAPP_RESTART_DELAY_SHELL_MS 100U
#define THEAPP_RESTART_DELAY_DHCPD_MS 1000U

typedef struct theapp_service
{
    const char* name;
    const char* path;
    char* const* argv;
    bool respawn;
    unsigned int restart_delay_ms;
    pid_t pid;
} theapp_service_t;

static char* const TheApp_shell_argv[] = {
    (char*) "TheShell",
    NULL
};

static char* const TheApp_dhcpd_argv[] = {
    (char*) "TheDHCPd",
    (char*) "--foreground",
    NULL
};

static void theapp_prepare_runtime_dirs(void)
{
    (void) mkdir("/var", 0755U);
    (void) mkdir("/var/log", 0755U);
    (void) mkdir("/var/run", 0755U);
}

static pid_t theapp_spawn_service(theapp_service_t* service)
{
    if (!service || !service->path || !service->argv)
        return (pid_t) -1;

    pid_t pid = fork();
    if (pid < 0)
        return (pid_t) -1;

    if (pid == 0)
    {
        (void) execv(service->path, service->argv);
        printf("[TheApp] execv('%s') failed errno=%d\n", service->path, errno);
        _exit(127);
    }

    service->pid = pid;
    return pid;
}

static theapp_service_t* theapp_find_service_by_pid(theapp_service_t* services,
                                                    size_t service_count,
                                                    pid_t pid)
{
    if (!services || pid <= 0)
        return NULL;

    for (size_t i = 0; i < service_count; i++)
    {
        if (services[i].pid == pid)
            return &services[i];
    }
    return NULL;
}

static void theapp_start_service_if_needed(theapp_service_t* service)
{
    if (!service || service->pid > 0)
        return;

    pid_t pid = theapp_spawn_service(service);
    if (pid <= 0)
    {
        printf("[TheApp] failed to spawn service '%s' path='%s' errno=%d\n",
               service->name ? service->name : "?",
               service->path ? service->path : "?",
               errno);
        return;
    }

    printf("[TheApp] service '%s' started pid=%d path='%s'\n",
           service->name ? service->name : "?",
           (int) pid,
           service->path ? service->path : "?");
}

static void theapp_start_all_services(theapp_service_t* services, size_t service_count)
{
    if (!services)
        return;
    for (size_t i = 0; i < service_count; i++)
        theapp_start_service_if_needed(&services[i]);
}

int main(int argc, char** argv, char** envp)
{
    (void) argc;
    (void) argv;
    (void) envp;

    theapp_prepare_runtime_dirs();

    theapp_service_t services[] = {
        {
            .name = "TheDHCPd",
            .path = "/drv/TheDHCPd",
            .argv = TheApp_dhcpd_argv,
            .respawn = true,
            .restart_delay_ms = THEAPP_RESTART_DELAY_DHCPD_MS,
            .pid = 0
        },
        {
            .name = "TheShell",
            .path = "/bin/TheShell",
            .argv = TheApp_shell_argv,
            .respawn = true,
            .restart_delay_ms = THEAPP_RESTART_DELAY_SHELL_MS,
            .pid = 0
        }
    };

    const size_t service_count = sizeof(services) / sizeof(services[0]);
    theapp_start_all_services(services, service_count);

    for (;;)
    {
        int status = 0;
        pid_t dead_pid = waitpid(-1, &status, 0);
        if (dead_pid < 0)
        {
            if (errno == ECHILD)
                theapp_start_all_services(services, service_count);
            (void) usleep(200U * 1000U);
            continue;
        }

        theapp_service_t* service = theapp_find_service_by_pid(services, service_count, dead_pid);
        if (!service)
            continue;

        service->pid = 0;
        if (WIFEXITED(status))
        {
            printf("[TheApp] service '%s' exited pid=%d code=%d\n",
                   service->name,
                   (int) dead_pid,
                   WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            printf("[TheApp] service '%s' terminated pid=%d sig=%d\n",
                   service->name,
                   (int) dead_pid,
                   WTERMSIG(status));
        }
        else
        {
            printf("[TheApp] service '%s' ended pid=%d status=0x%X\n",
                   service->name,
                   (int) dead_pid,
                   (unsigned int) status);
        }

        if (!service->respawn)
            continue;

        if (service->restart_delay_ms != 0U)
            (void) usleep(service->restart_delay_ms * 1000U);
        theapp_start_service_if_needed(service);
    }
}
