#ifndef THEAPP_H
#define THEAPP_H

#include <stdint.h>

#define THEAPP_SPAWN_MAGIC    0x54415350U
#define THEAPP_SPAWN_SOCK     "/var/run/theapp_spawn.sock"
#define THEAPP_SPAWN_PATH_MAX 256U
#define THEAPP_SPAWN_ARGS_MAX 512U

typedef struct theapp_spawn_msg
{
    uint32_t magic;
    uint32_t argc;
    char payload[THEAPP_SPAWN_PATH_MAX + THEAPP_SPAWN_ARGS_MAX];
} theapp_spawn_msg_t;

int theapp_spawn(const char* path);
int theapp_spawn_argv(const char* path, char* const argv[]);

#endif
