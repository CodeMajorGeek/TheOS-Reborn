#include <theapp.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int theapp_send_msg(const theapp_spawn_msg_t* msg, size_t payload_len)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(THEAPP_SPAWN_SOCK);
    if (path_len >= sizeof(addr.sun_path))
    {
        (void) close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, THEAPP_SPAWN_SOCK, path_len + 1);

    size_t total = sizeof(uint32_t) * 2 + payload_len;
    ssize_t sent = sendto(fd, msg, total, 0,
                          (const struct sockaddr*) &addr,
                          (socklen_t) (sizeof(sa_family_t) + path_len + 1));
    (void) close(fd);

    if (sent < 0 || (size_t) sent != total)
    {
        if (sent >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

int theapp_spawn(const char* path)
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len + 1 > THEAPP_SPAWN_PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    theapp_spawn_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = THEAPP_SPAWN_MAGIC;
    msg.argc = 0;
    memcpy(msg.payload, path, path_len + 1);

    return theapp_send_msg(&msg, path_len + 1);
}

int theapp_spawn_argv(const char* path, char* const argv[])
{
    if (!path || path[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len + 1 > THEAPP_SPAWN_PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    theapp_spawn_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = THEAPP_SPAWN_MAGIC;

    size_t offset = 0;
    memcpy(msg.payload + offset, path, path_len + 1);
    offset += path_len + 1;

    uint32_t argc = 0;
    if (argv)
    {
        for (uint32_t i = 0; argv[i]; i++)
        {
            size_t arg_len = strlen(argv[i]);
            if (offset + arg_len + 1 > sizeof(msg.payload))
            {
                errno = E2BIG;
                return -1;
            }
            memcpy(msg.payload + offset, argv[i], arg_len + 1);
            offset += arg_len + 1;
            argc++;
        }
    }

    msg.argc = argc;
    return theapp_send_msg(&msg, offset);
}
