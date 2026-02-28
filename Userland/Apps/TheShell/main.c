#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

#define SHELL_LINE_MAX           256U
#define SHELL_PATH_MAX           256U
#define SHELL_CAT_MAX            4096U
#define SHELL_MAX_COMPONENTS     32U
#define SHELL_MAX_COMPONENT_LEN  255U

static char* shell_trim(char* str)
{
    if (!str)
        return NULL;

    while (*str == ' ' || *str == '\t')
        str++;

    size_t len = strlen(str);
    while (len > 0U && (str[len - 1U] == ' ' || str[len - 1U] == '\t'))
    {
        str[len - 1U] = '\0';
        len--;
    }

    return str;
}

static char* shell_find_char(char* str, char needle)
{
    if (!str)
        return NULL;

    while (*str != '\0')
    {
        if (*str == needle)
            return str;
        str++;
    }

    return NULL;
}

static bool shell_normalize_absolute(const char* input, char* out, size_t out_size)
{
    if (!input || !out || out_size < 2U || input[0] != '/')
        return false;

    char components[SHELL_MAX_COMPONENTS][SHELL_MAX_COMPONENT_LEN + 1U];
    size_t component_count = 0;

    const char* cursor = input;
    while (*cursor != '\0')
    {
        while (*cursor == '/')
            cursor++;

        if (*cursor == '\0')
            break;

        char token[SHELL_MAX_COMPONENT_LEN + 2U];
        size_t token_len = 0;
        while (cursor[token_len] != '\0' && cursor[token_len] != '/')
        {
            if (token_len >= SHELL_MAX_COMPONENT_LEN)
                return false;

            token[token_len] = cursor[token_len];
            token_len++;
        }

        token[token_len] = '\0';
        cursor += token_len;

        if (strcmp(token, ".") == 0)
            continue;
        if (strcmp(token, "..") == 0)
        {
            if (component_count > 0U)
                component_count--;
            continue;
        }

        if (component_count >= SHELL_MAX_COMPONENTS)
            return false;

        strcpy(components[component_count], token);
        component_count++;
    }

    size_t len = 0;
    out[len++] = '/';
    out[len] = '\0';

    for (size_t i = 0; i < component_count; i++)
    {
        size_t part_len = strlen(components[i]);
        if (len + part_len + 1U >= out_size)
            return false;

        if (len > 1U)
            out[len++] = '/';

        memcpy(out + len, components[i], part_len);
        len += part_len;
        out[len] = '\0';
    }

    return true;
}

static bool shell_resolve_path(const char* cwd, const char* input, char* out, size_t out_size)
{
    if (!cwd || !input || !out || out_size == 0U)
        return false;

    char joined[SHELL_PATH_MAX];
    if (input[0] == '/')
    {
        size_t input_len = strlen(input);
        if (input_len >= sizeof(joined))
            return false;

        memcpy(joined, input, input_len + 1U);
    }
    else
    {
        size_t cwd_len = strlen(cwd);
        size_t input_len = strlen(input);
        if (cwd_len == 0U || cwd[0] != '/')
            return false;

        if (strcmp(cwd, "/") == 0)
        {
            if (input_len + 1U >= sizeof(joined))
                return false;

            joined[0] = '/';
            memcpy(joined + 1U, input, input_len + 1U);
        }
        else
        {
            if (cwd_len + input_len + 1U >= sizeof(joined))
                return false;

            memcpy(joined, cwd, cwd_len);
            joined[cwd_len] = '/';
            memcpy(joined + cwd_len + 1U, input, input_len + 1U);
        }
    }

    return shell_normalize_absolute(joined, out, out_size);
}

static bool shell_command_is_path(const char* command)
{
    if (!command)
        return false;

    while (*command != '\0')
    {
        if (*command == '/')
            return true;
        command++;
    }

    return false;
}

static const char* shell_signal_name(int signal)
{
    switch (signal)
    {
        case SYS_SIGFPE:
            return "SIGFPE";
        case SYS_SIGTRAP:
            return "SIGTRAP";
        case SYS_SIGILL:
            return "SIGILL";
        case SYS_SIGSEGV:
            return "SIGSEGV";
        case SYS_SIGFAULT:
            return "SIGFAULT";
        default:
            return "SIGUNKNOWN";
    }
}

static void shell_cmd_exec_path(const char* cwd, const char* command)
{
    if (!cwd || !command || command[0] == '\0')
        return;

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, command, resolved, sizeof(resolved)))
    {
        printf("exec: invalid path\n");
        return;
    }

    int fork_rc = sys_fork();
    if (fork_rc < 0)
    {
        printf("exec: fork failed (rc=%d)\n", fork_rc);
        return;
    }

    if (fork_rc == 0)
    {
        const char* const argv[] = { resolved, NULL };
        const char* const envp[] = { NULL };
        int rc = sys_execve(resolved, argv, envp);
        printf("exec: cannot run '%s' (rc=%d)\n", resolved, rc);
        sys_exit(127);
    }

    for (;;)
    {
        int status = 0;
        int signal = 0;
        int wait_rc = sys_waitpid(fork_rc, &status, &signal);
        if (wait_rc == fork_rc)
        {
            if (signal != 0)
                printf("exec: pid=%d killed by %s\n", fork_rc, shell_signal_name(signal));
            else if (status != 0)
                printf("exec: pid=%d exited status=%d\n", fork_rc, status);
            return;
        }

        if (wait_rc < 0)
        {
            printf("exec: waitpid failed for pid=%d\n", fork_rc);
            return;
        }

        (void) sys_sleep_ms(10);
    }
}

static void shell_cmd_pwd(const char* cwd)
{
    printf("%s\n", cwd);
}

static void shell_cmd_cd(char* cwd, const char* arg)
{
    const char* target = (arg && arg[0] != '\0') ? arg : "/";
    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, target, resolved, sizeof(resolved)))
    {
        printf("cd: invalid path\n");
        return;
    }

    if (fs_is_dir(resolved) != 1)
    {
        printf("cd: no such directory: %s\n", resolved);
        return;
    }

    strcpy(cwd, resolved);
}

static void shell_cmd_ls(const char* cwd, const char* arg)
{
    char resolved[SHELL_PATH_MAX];
    const char* target = cwd;
    if (arg && arg[0] != '\0')
    {
        if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
        {
            printf("ls: invalid path\n");
            return;
        }

        target = resolved;
    }

    if (fs_ls_path(target) != 0)
        printf("ls: cannot access '%s'\n", target);
}

static void shell_cmd_cat(const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        printf("cat: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        printf("cat: invalid path\n");
        return;
    }

    static uint8_t buffer[SHELL_CAT_MAX];
    size_t out_size = 0;
    if (fs_read(resolved, buffer, sizeof(buffer) - 1U, &out_size) != 0)
    {
        printf("cat: cannot read '%s'\n", resolved);
        return;
    }

    buffer[out_size] = '\0';
    if (out_size > 0U)
        (void) sys_console_write(buffer, out_size);
    if (out_size == 0U || buffer[out_size - 1U] != '\n')
        putc('\n');
}

static void shell_cmd_touch(const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        printf("touch: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        printf("touch: invalid path\n");
        return;
    }

    int fd = sys_open(resolved, SYS_OPEN_WRITE | SYS_OPEN_CREATE);
    if (fd < 0)
    {
        printf("touch: cannot touch '%s'\n", resolved);
        return;
    }

    if (sys_close(fd) != 0)
        printf("touch: close failed for '%s'\n", resolved);
}

static void shell_cmd_mkdir(const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        printf("mkdir: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        printf("mkdir: invalid path\n");
        return;
    }

    if (fs_mkdir(resolved) != 0)
        printf("mkdir: cannot create directory '%s'\n", resolved);
}

static void shell_cmd_echo(const char* cwd, char* arg)
{
    if (!arg)
    {
        putc('\n');
        return;
    }

    char* trimmed_arg = shell_trim(arg);
    if (!trimmed_arg || trimmed_arg[0] == '\0')
    {
        putc('\n');
        return;
    }

    char* pipe = shell_find_char(trimmed_arg, '|');
    if (!pipe)
    {
        printf("%s\n", trimmed_arg);
        return;
    }

    *pipe = '\0';
    char* text = shell_trim(trimmed_arg);
    char* target = shell_trim(pipe + 1U);
    if (!target || target[0] == '\0')
    {
        printf("echo: missing target path after '|'\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, target, resolved, sizeof(resolved)))
    {
        printf("echo: invalid target path\n");
        return;
    }

    int fd = sys_open(resolved, SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_TRUNC);
    if (fd < 0)
    {
        printf("echo: cannot open '%s'\n", resolved);
        return;
    }

    const char* out_text = text ? text : "";
    size_t text_len = strlen(out_text);
    if (text_len > 0U && fs_write(fd, out_text, text_len) != (int) text_len)
    {
        printf("echo: write failed on '%s'\n", resolved);
        (void) sys_close(fd);
        return;
    }

    const char newline = '\n';
    if (fs_write(fd, &newline, 1U) != 1)
    {
        printf("echo: write failed on '%s'\n", resolved);
        (void) sys_close(fd);
        return;
    }

    if (sys_close(fd) != 0)
        printf("echo: close failed on '%s'\n", resolved);
}

static void shell_print_help(void)
{
    printf("Commands:\n");
    printf("  pwd\n");
    printf("  cd <path>\n");
    printf("  ls [path]\n");
    printf("  cat <path>\n");
    printf("  touch <path>\n");
    printf("  mkdir <path>\n");
    printf("  echo <text> | <path>\n");
    printf("  <path/to/binary> (ex: ./bin/TheTest)\n");
    printf("  help\n");
    printf("  exit\n");
}

int main(void)
{
    if (keyboard_load_config("/system/keyboard.conf") != 0)
        printf("[TheShell] keyboard config unavailable, fallback=qwerty\n");

    char cwd[SHELL_PATH_MAX];
    strcpy(cwd, "/");

    printf("TheShell started.\n");
    printf("Type 'help' for commands.\n");

    for (;;)
    {
        printf("TheShell:%s$ ", cwd);

        char line[SHELL_LINE_MAX];
        if (!fgets(line, (int) sizeof(line)))
            continue;
        size_t line_len = strlen(line);
        if (line_len > 0U && line[line_len - 1U] == '\n')
            line[line_len - 1U] = '\0';

        char* command = shell_trim(line);
        if (!command || command[0] == '\0')
            continue;

        char* arg = NULL;
        size_t cmd_len = 0;
        while (command[cmd_len] != '\0' && command[cmd_len] != ' ' && command[cmd_len] != '\t')
            cmd_len++;
        if (command[cmd_len] != '\0')
        {
            command[cmd_len] = '\0';
            arg = shell_trim(command + cmd_len + 1U);
        }

        if (strcmp(command, "pwd") == 0)
            shell_cmd_pwd(cwd);
        else if (strcmp(command, "cd") == 0)
            shell_cmd_cd(cwd, arg);
        else if (strcmp(command, "ls") == 0)
            shell_cmd_ls(cwd, arg);
        else if (strcmp(command, "cat") == 0)
            shell_cmd_cat(cwd, arg);
        else if (strcmp(command, "touch") == 0)
            shell_cmd_touch(cwd, arg);
        else if (strcmp(command, "mkdir") == 0)
            shell_cmd_mkdir(cwd, arg);
        else if (strcmp(command, "echo") == 0)
            shell_cmd_echo(cwd, arg);
        else if (strcmp(command, "help") == 0)
            shell_print_help();
        else if (strcmp(command, "exit") == 0)
        {
            printf("Bye !\n");
            sys_exit(0);
        }
        else if (shell_command_is_path(command))
            shell_cmd_exec_path(cwd, command);
        else
            printf("unknown command: %s\n", command);
    }
}
