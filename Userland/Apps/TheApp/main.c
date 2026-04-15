#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <theapp.h>
#include <unistd.h>

#define THEAPP_RESTART_DELAY_SHELL_MS 100U
#define THEAPP_RESTART_DELAY_DHCPD_MS 1000U
#define THEAPP_IDLE_SLEEP_US          10000U

/* ------------------------------------------------------------------ */
/*  Service management                                                 */
/* ------------------------------------------------------------------ */

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

static char* const TheApp_windowserver_argv[] = {
    (char*) "TheWindowServer",
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

/* ------------------------------------------------------------------ */
/*  Spawn server (AF_UNIX SOCK_DGRAM)                                  */
/* ------------------------------------------------------------------ */

static int theapp_spawn_server_init(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        printf("[TheApp] spawn server socket failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(THEAPP_SPAWN_SOCK);
    if (path_len >= sizeof(addr.sun_path))
    {
        printf("[TheApp] spawn server path too long\n");
        (void) close(fd);
        return -1;
    }
    memcpy(addr.sun_path, THEAPP_SPAWN_SOCK, path_len + 1);

    if (bind(fd, (const struct sockaddr*) &addr,
             (socklen_t) (sizeof(sa_family_t) + path_len + 1)) < 0)
    {
        printf("[TheApp] spawn server bind failed errno=%d\n", errno);
        (void) close(fd);
        return -1;
    }

    printf("[TheApp] spawn server listening on %s\n", THEAPP_SPAWN_SOCK);
    return fd;
}

static void theapp_spawn_server_poll(int fd)
{
    theapp_spawn_msg_t msg;

    for (;;)
    {
        ssize_t n = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT);
        if (n < (ssize_t) (sizeof(uint32_t) * 2))
        {
            if (n < 0 && errno != EAGAIN)
                printf("[TheApp] spawn recv error n=%d errno=%d\n", (int) n, errno);
            return;
        }

        if (msg.magic != THEAPP_SPAWN_MAGIC)
        {
            printf("[TheApp] spawn bad magic 0x%08X\n", (unsigned int) msg.magic);
            continue;
        }

        size_t payload_len = (size_t) n - sizeof(uint32_t) * 2;
        if (payload_len == 0)
        {
            printf("[TheApp] spawn empty payload\n");
            continue;
        }

        msg.payload[sizeof(msg.payload) - 1] = '\0';

        const char* path = msg.payload;
        size_t path_len = strlen(path);
        if (path_len == 0 || path[0] != '/')
        {
            printf("[TheApp] spawn invalid path\n");
            continue;
        }

        char* argv_ptrs[32];
        uint32_t argc = 0;

        if (msg.argc == 0)
        {
            const char* basename = path;
            for (const char* p = path; *p; p++)
            {
                if (*p == '/')
                    basename = p + 1;
            }
            argv_ptrs[0] = (char*) basename;
            argc = 1;
        }
        else
        {
            size_t off = path_len + 1;
            for (uint32_t i = 0; i < msg.argc && argc < 31; i++)
            {
                if (off >= payload_len)
                    break;
                argv_ptrs[argc++] = &msg.payload[off];
                off += strlen(&msg.payload[off]) + 1;
            }
        }
        argv_ptrs[argc] = NULL;

        printf("[TheApp] spawn request path='%s' argc=%u\n",
               path, (unsigned int) argc);

        pid_t pid = fork();
        if (pid < 0)
        {
            printf("[TheApp] spawn fork failed errno=%d\n", errno);
            continue;
        }

        if (pid == 0)
        {
            (void) execv(path, argv_ptrs);
            printf("[TheApp] spawn execv('%s') failed errno=%d\n", path, errno);
            _exit(127);
        }

        printf("[TheApp] spawned pid=%d path='%s'\n", (int) pid, path);
    }
}

/* ------------------------------------------------------------------ */
/*  Reap children and respawn managed services                         */
/* ------------------------------------------------------------------ */

static void theapp_reap_and_respawn(theapp_service_t* services, size_t service_count)
{
    for (;;)
    {
        int status = 0;
        pid_t dead_pid = waitpid(-1, &status, WNOHANG);
        if (dead_pid <= 0)
            return;

        theapp_service_t* service = theapp_find_service_by_pid(services,
                                                               service_count,
                                                               dead_pid);
        if (!service)
        {
            if (WIFEXITED(status))
            {
                printf("[TheApp] unmanaged pid=%d exited code=%d\n",
                       (int) dead_pid, WEXITSTATUS(status));
            }
            else if (WIFSIGNALED(status))
            {
                printf("[TheApp] unmanaged pid=%d signal=%d\n",
                       (int) dead_pid, WTERMSIG(status));
            }
            continue;
        }

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

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv, char** envp)
{
    (void) argc;
    (void) argv;
    (void) envp;

    theapp_prepare_runtime_dirs();

    theapp_service_t services[] = {
        {
            .name = "TheShell",
            .path = "/bin/TheShell",
            .argv = TheApp_shell_argv,
            .respawn = true,
            .restart_delay_ms = THEAPP_RESTART_DELAY_SHELL_MS,
            .pid = 0
        },
        {
            .name = "TheDHCPd",
            .path = "/drv/TheDHCPd",
            .argv = TheApp_dhcpd_argv,
            .respawn = true,
            .restart_delay_ms = THEAPP_RESTART_DELAY_DHCPD_MS,
            .pid = 0
        },
        {
            .name = "TheWindowServer",
            .path = "/bin/TheWindowServer",
            .argv = TheApp_windowserver_argv,
            .respawn = false,
            .restart_delay_ms = 0U,
            .pid = 0
        }
    };

    const size_t service_count = sizeof(services) / sizeof(services[0]);

    int spawn_fd = theapp_spawn_server_init();

    theapp_start_all_services(services, service_count);

    for (;;)
    {
        if (spawn_fd >= 0)
            theapp_spawn_server_poll(spawn_fd);

        theapp_reap_and_respawn(services, service_count);
        (void) usleep(THEAPP_IDLE_SLEEP_US);
    }
}
