#include "shell_core.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>

#define SHELL_LINE_MAX           256U
#define SHELL_PATH_MAX           THESHELL_PATH_MAX
#define SHELL_CAT_MAX            4096U
#define SHELL_MAX_COMPONENTS     32U
#define SHELL_MAX_COMPONENT_LEN  255U
#define SHELL_EXEC_MAX_ARGS      32U
#define SHELL_AUTOCOMPLETE_MAX   64U

static const char* shell_builtin_commands[] =
{
    "pwd",
    "cd",
    "ls",
    "cat",
    "touch",
    "mkdir",
    "echo",
    "clear",
    "help",
    "exit"
};

typedef struct shell_command_alias
{
    const char* alias;
    const char* target;
} shell_command_alias_t;

static const char* shell_known_binaries[] =
{
    "TheApp",
    "TheShell",
    "TheTest",
    "ThePowerManager",
    "TheSystemMonitor",
    "TheSystemMonitorGUI",
    "TheWindowServer",
    "TheShellGUI",
    "TheMicroPython",
    "MicroPython"
};

static const shell_command_alias_t shell_command_aliases[] =
{
    { "doom", "embeddedDOOM" },
    { "micropython", "TheMicroPython" },
    { "mpy", "TheMicroPython" },
    { "windowserver", "TheWindowServer" }
};

static void shell_output_raw(theshell_core_t* core, const char* text, size_t len)
{
    if (!core || !text || len == 0U)
        return;

    if (core->io.write)
    {
        core->io.write(core->io.opaque, text, len);
        return;
    }

    (void) write(STDOUT_FILENO, text, len);
}

static void shell_output_cstr(theshell_core_t* core, const char* text)
{
    if (!text)
        return;

    shell_output_raw(core, text, strlen(text));
}

static void shell_output_printf(theshell_core_t* core, const char* format, ...)
{
    if (!format)
        return;

    char buffer[768];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    if (len <= 0)
        return;

    size_t used = (size_t) len;
    if (used >= sizeof(buffer))
        used = sizeof(buffer) - 1U;

    shell_output_raw(core, buffer, used);
}

static void shell_drain_console_capture_sid(theshell_core_t* core, uint32_t console_sid)
{
    if (!core || console_sid == 0U)
        return;

    char capture[512];
    for (;;)
    {
        int rc = sys_console_route_read_sid(console_sid, capture, sizeof(capture));
        if (rc <= 0)
            break;
        shell_output_raw(core, capture, (size_t) rc);
    }
}

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

static bool shell_path_exists(const char* path)
{
    if (!path || path[0] == '\0')
        return false;

    struct stat st;
    return stat(path, &st) == 0;
}

static bool shell_path_is_dir(const char* path)
{
    if (!path || path[0] == '\0')
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;

    return S_ISDIR(st.st_mode);
}

static bool shell_build_the_alias_path(const char* command, char* out, size_t out_size)
{
    if (!command || !out || out_size < 7U)
        return false;

    const char* base = command;
    if (strncasecmp(command, "the", 3U) == 0 && command[3] != '\0')
        base = command + 3U;

    if (base[0] == '\0')
        return false;

    size_t pos = 0;
    const char prefix[] = "/bin/The";
    const size_t prefix_len = sizeof(prefix) - 1U;
    if (prefix_len + 2U > out_size)
        return false;

    memcpy(out, prefix, prefix_len);
    pos += prefix_len;

    bool first_alpha = true;
    for (size_t i = 0U; base[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char) base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        {
            if (first_alpha && c >= 'a' && c <= 'z')
                c = (unsigned char) (c - ('a' - 'A'));
            else if (!first_alpha && c >= 'A' && c <= 'Z')
                c = (unsigned char) (c + ('a' - 'A'));

            first_alpha = false;
            if (pos + 1U >= out_size)
                return false;
            out[pos++] = (char) c;
            continue;
        }

        if (c == '-' || c == '_')
            continue;

        if (pos + 1U >= out_size)
            return false;
        out[pos++] = (char) c;
    }

    if (pos >= out_size)
        return false;

    out[pos] = '\0';
    return true;
}

static bool shell_command_key(const char* command, char* out, size_t out_size)
{
    if (!command || !out || out_size == 0U)
        return false;

    size_t len = 0U;
    while (command[len] != '\0')
    {
        if (len + 1U >= out_size)
            return false;

        out[len] = (char) tolower((unsigned char) command[len]);
        len++;
    }

    out[len] = '\0';
    return len != 0U;
}

static bool shell_build_known_alias_path(const char* command, char* out, size_t out_size)
{
    if (!command || !out)
        return false;

    char command_key[SHELL_PATH_MAX];
    if (!shell_command_key(command, command_key, sizeof(command_key)))
        return false;

    for (size_t i = 0U; i < (sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0])); i++)
    {
        char known_key[SHELL_PATH_MAX];
        if (!shell_command_key(shell_known_binaries[i], known_key, sizeof(known_key)))
            continue;

        if (strcmp(command_key, known_key) != 0)
            continue;

        int len = snprintf(out, out_size, "/bin/%s", shell_known_binaries[i]);
        return len > 0 && (size_t) len < out_size;
    }

    return false;
}

static bool shell_build_custom_alias_path(const char* command, char* out, size_t out_size)
{
    if (!command || !out)
        return false;

    char command_key[SHELL_PATH_MAX];
    if (!shell_command_key(command, command_key, sizeof(command_key)))
        return false;

    for (size_t i = 0U; i < (sizeof(shell_command_aliases) / sizeof(shell_command_aliases[0])); i++)
    {
        char alias_key[SHELL_PATH_MAX];
        if (!shell_command_key(shell_command_aliases[i].alias, alias_key, sizeof(alias_key)))
            continue;

        if (strcmp(command_key, alias_key) != 0)
            continue;

        int len = snprintf(out, out_size, "/bin/%s", shell_command_aliases[i].target);
        return len > 0 && (size_t) len < out_size;
    }

    return false;
}

static bool shell_resolve_exec_command(const char* cwd, const char* command, char* out, size_t out_size)
{
    if (!cwd || !command || !out || out_size == 0U || command[0] == '\0')
        return false;

    if (shell_command_is_path(command))
        return shell_resolve_path(cwd, command, out, out_size);

    int len = snprintf(out, out_size, "/bin/%s", command);
    if (len > 0 && (size_t) len < out_size && shell_path_exists(out))
        return true;

    len = snprintf(out, out_size, "/drv/%s", command);
    if (len > 0 && (size_t) len < out_size && shell_path_exists(out))
        return true;

    char custom_alias_path[SHELL_PATH_MAX];
    if (shell_build_custom_alias_path(command, custom_alias_path, sizeof(custom_alias_path)) &&
        shell_path_exists(custom_alias_path))
    {
        size_t alias_len = strlen(custom_alias_path);
        if (alias_len + 1U > out_size)
            return false;

        memcpy(out, custom_alias_path, alias_len + 1U);
        return true;
    }

    char known_alias_path[SHELL_PATH_MAX];
    if (shell_build_known_alias_path(command, known_alias_path, sizeof(known_alias_path)) &&
        shell_path_exists(known_alias_path))
    {
        size_t alias_len = strlen(known_alias_path);
        if (alias_len + 1U > out_size)
            return false;

        memcpy(out, known_alias_path, alias_len + 1U);
        return true;
    }

    char alias_path[SHELL_PATH_MAX];
    if (shell_build_the_alias_path(command, alias_path, sizeof(alias_path)) && shell_path_exists(alias_path))
    {
        size_t alias_len = strlen(alias_path);
        if (alias_len + 1U > out_size)
            return false;

        memcpy(out, alias_path, alias_len + 1U);
        return true;
    }

    return false;
}

static const char* shell_signal_name(int signal)
{
    switch (signal)
    {
        case SIGHUP: return "SIGHUP";
        case SIGINT: return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL: return "SIGILL";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGUSR1: return "SIGUSR1";
        case SIGSEGV: return "SIGSEGV";
        case SIGUSR2: return "SIGUSR2";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
        case SIGCHLD: return "SIGCHLD";
        case SIGCONT: return "SIGCONT";
        case SIGSTOP: return "SIGSTOP";
        case SIGTSTP: return "SIGTSTP";
        case SIGTTIN: return "SIGTTIN";
        case SIGTTOU: return "SIGTTOU";
        default: return "SIG?";
    }
}

static size_t shell_split_words(char* text, char* out_words[], size_t max_words)
{
    if (!text || !out_words || max_words == 0U)
        return 0U;

    size_t count = 0U;
    char* cursor = text;

    while (*cursor != '\0' && count < max_words)
    {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;

        if (*cursor == '\0')
            break;

        out_words[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
            cursor++;

        if (*cursor == '\0')
            break;

        *cursor = '\0';
        cursor++;
    }

    return count;
}

static bool shell_read_file(const char* path, uint8_t* buf, size_t buf_size, size_t* out_size)
{
    if (!path || !buf || buf_size == 0U || !out_size)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    size_t total = 0U;
    while (total < buf_size)
    {
        ssize_t rc = read(fd, buf + total, buf_size - total);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            (void) close(fd);
            return false;
        }

        if (rc == 0)
            break;

        total += (size_t) rc;
    }

    (void) close(fd);
    *out_size = total;
    return true;
}

static bool shell_starts_with_nocase(const char* text, const char* prefix)
{
    if (!text || !prefix)
        return false;

    size_t i = 0U;
    while (prefix[i] != '\0')
    {
        if (text[i] == '\0')
            return false;
        if (tolower((unsigned char) text[i]) != tolower((unsigned char) prefix[i]))
            return false;
        i++;
    }

    return true;
}

static bool shell_split_path_prefix(const char* token,
                                    char* out_dir,
                                    size_t out_dir_size,
                                    char* out_name_prefix,
                                    size_t out_name_prefix_size)
{
    if (!token || !out_dir || !out_name_prefix || out_dir_size == 0U || out_name_prefix_size == 0U)
        return false;

    const char* slash = NULL;
    for (const char* p = token; *p != '\0'; p++)
    {
        if (*p == '/')
            slash = p;
    }

    if (!slash)
    {
        if (out_dir_size < 2U)
            return false;
        out_dir[0] = '.';
        out_dir[1] = '\0';

        size_t len = strlen(token);
        if (len + 1U > out_name_prefix_size)
            return false;
        memcpy(out_name_prefix, token, len + 1U);
        return true;
    }

    size_t dir_len = (size_t) (slash - token);
    if (dir_len == 0U)
    {
        if (out_dir_size < 2U)
            return false;
        out_dir[0] = '/';
        out_dir[1] = '\0';
    }
    else
    {
        if (dir_len + 1U > out_dir_size)
            return false;
        memcpy(out_dir, token, dir_len);
        out_dir[dir_len] = '\0';
    }

    size_t prefix_len = strlen(slash + 1U);
    if (prefix_len + 1U > out_name_prefix_size)
        return false;
    memcpy(out_name_prefix, slash + 1U, prefix_len + 1U);
    return true;
}

static void shell_tab_complete_argument_path(const char* cwd,
                                             const char* token,
                                             char out_matches[][SHELL_PATH_MAX],
                                             bool out_is_dir[],
                                             size_t max_matches,
                                             size_t* out_count)
{
    if (!cwd || !token || !out_matches || !out_is_dir || !out_count || max_matches == 0U)
        return;

    *out_count = 0U;

    char dir_part[SHELL_PATH_MAX];
    char name_prefix[SHELL_PATH_MAX];
    if (!shell_split_path_prefix(token, dir_part, sizeof(dir_part), name_prefix, sizeof(name_prefix)))
        return;

    char resolved_dir[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, dir_part, resolved_dir, sizeof(resolved_dir)))
        return;

    DIR* dir = opendir(resolved_dir);
    if (!dir)
        return;

    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!shell_starts_with_nocase(ent->d_name, name_prefix))
            continue;
        if (*out_count >= max_matches)
            break;

        bool is_dir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN)
        {
            char abs_path[SHELL_PATH_MAX];
            if (strcmp(resolved_dir, "/") == 0)
                snprintf(abs_path, sizeof(abs_path), "/%s", ent->d_name);
            else
                snprintf(abs_path, sizeof(abs_path), "%s/%s", resolved_dir, ent->d_name);
            is_dir = shell_path_is_dir(abs_path);
        }

        if (strcmp(dir_part, ".") == 0)
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "%s", ent->d_name);
        else if (strcmp(dir_part, "/") == 0)
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "/%s", ent->d_name);
        else
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "%s/%s", dir_part, ent->d_name);

        out_is_dir[*out_count] = is_dir;
        (*out_count)++;
    }

    (void) closedir(dir);
}

static size_t shell_collect_command_matches(const char* prefix,
                                            const char* out_matches[],
                                            size_t max_matches)
{
    if (!prefix || !out_matches || max_matches == 0U)
        return 0U;

    static char alias_names[sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0])][SHELL_PATH_MAX];
    size_t count = 0U;

    #define SHELL_ADD_MATCH_UNIQUE(candidate)                                                    \
        do                                                                                       \
        {                                                                                        \
            const char* _candidate = (candidate);                                                \
            bool _dup = false;                                                                   \
            size_t _safe_count = (count < max_matches) ? count : max_matches;                   \
            for (size_t _j = 0U; _j < _safe_count; _j++)                                         \
            {                                                                                    \
                if (strcasecmp(out_matches[_j], _candidate) == 0)                                \
                {                                                                                \
                    _dup = true;                                                                 \
                    break;                                                                       \
                }                                                                                \
            }                                                                                    \
            if (!_dup)                                                                           \
            {                                                                                    \
                if (count < max_matches)                                                         \
                    out_matches[count] = _candidate;                                             \
                count++;                                                                         \
            }                                                                                    \
        } while (0)

    for (size_t i = 0U; i < sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]); i++)
    {
        if (shell_starts_with_nocase(shell_builtin_commands[i], prefix))
            SHELL_ADD_MATCH_UNIQUE(shell_builtin_commands[i]);
    }

    for (size_t i = 0U; i < sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0]); i++)
    {
        if (shell_starts_with_nocase(shell_known_binaries[i], prefix))
            SHELL_ADD_MATCH_UNIQUE(shell_known_binaries[i]);

        if (shell_command_key(shell_known_binaries[i], alias_names[i], sizeof(alias_names[i])) &&
            shell_starts_with_nocase(alias_names[i], prefix))
        {
            SHELL_ADD_MATCH_UNIQUE(alias_names[i]);
        }
    }

    for (size_t i = 0U; i < sizeof(shell_command_aliases) / sizeof(shell_command_aliases[0]); i++)
    {
        if (shell_starts_with_nocase(shell_command_aliases[i].alias, prefix))
            SHELL_ADD_MATCH_UNIQUE(shell_command_aliases[i].alias);
    }

    #undef SHELL_ADD_MATCH_UNIQUE

    return count;
}

int theshell_core_autocomplete(theshell_core_t* core,
                               char* inout_line,
                               size_t line_size,
                               char out_matches[][THESHELL_PATH_MAX],
                               bool out_match_is_dir[],
                               size_t max_matches,
                               size_t* out_match_count)
{
    if (!core || !inout_line || line_size < 2U)
    {
        errno = EINVAL;
        return -1;
    }

    if (out_match_count)
        *out_match_count = 0U;

    size_t len = strlen(inout_line);
    size_t cursor = len;
    if (cursor == 0U)
        return 0;

    size_t token_start = cursor;
    while (token_start > 0U)
    {
        char c = inout_line[token_start - 1U];
        if (c == ' ' || c == '\t')
            break;
        token_start--;
    }

    bool is_first_token = (token_start == 0U);
    for (size_t i = token_start; i < cursor; i++)
    {
        if (inout_line[i] == '/')
        {
            is_first_token = false;
            break;
        }
    }

    char prefix[SHELL_LINE_MAX];
    if (cursor < token_start || (cursor - token_start) >= sizeof(prefix))
        return 0;
    memcpy(prefix, inout_line + token_start, cursor - token_start);
    prefix[cursor - token_start] = '\0';

    if (!is_first_token)
    {
        char path_matches[SHELL_AUTOCOMPLETE_MAX][SHELL_PATH_MAX];
        bool path_is_dir[SHELL_AUTOCOMPLETE_MAX];
        size_t path_count = 0U;
        shell_tab_complete_argument_path(core->cwd,
                                         prefix,
                                         path_matches,
                                         path_is_dir,
                                         SHELL_AUTOCOMPLETE_MAX,
                                         &path_count);
        if (path_count == 0U)
            return 0;

        if (path_count == 1U)
        {
            char replacement[SHELL_PATH_MAX + 2U];
            size_t repl_len = strlen(path_matches[0]);
            if (repl_len + 2U > sizeof(replacement))
                return 0;
            memcpy(replacement, path_matches[0], repl_len + 1U);
            if (path_is_dir[0] && (repl_len == 0U || replacement[repl_len - 1U] != '/'))
            {
                replacement[repl_len++] = '/';
                replacement[repl_len] = '\0';
            }

            if (token_start + repl_len + 1U > line_size)
                return 0;
            memcpy(inout_line + token_start, replacement, repl_len);
            inout_line[token_start + repl_len] = '\0';
            return 1;
        }

        size_t copy_count = path_count;
        if (copy_count > max_matches)
            copy_count = max_matches;

        if (out_matches && out_match_is_dir)
        {
            for (size_t i = 0U; i < copy_count; i++)
            {
                (void) snprintf(out_matches[i], THESHELL_PATH_MAX, "%s", path_matches[i]);
                out_match_is_dir[i] = path_is_dir[i];
            }
        }

        if (out_match_count)
            *out_match_count = path_count;

        return 2;
    }

    const char* cmd_matches[SHELL_AUTOCOMPLETE_MAX];
    size_t cmd_count = shell_collect_command_matches(prefix, cmd_matches, SHELL_AUTOCOMPLETE_MAX);
    if (cmd_count == 0U)
        return 0;

    if (cmd_count == 1U)
    {
        size_t chosen_len = strlen(cmd_matches[0]);
        if (token_start + chosen_len + 1U > line_size)
            return 0;
        memcpy(inout_line + token_start, cmd_matches[0], chosen_len);
        inout_line[token_start + chosen_len] = '\0';
        return 1;
    }

    size_t copy_count = cmd_count;
    if (copy_count > max_matches)
        copy_count = max_matches;
    if (out_matches)
    {
        for (size_t i = 0U; i < copy_count; i++)
        {
            (void) snprintf(out_matches[i], THESHELL_PATH_MAX, "%s", cmd_matches[i]);
            if (out_match_is_dir)
                out_match_is_dir[i] = false;
        }
    }

    if (out_match_count)
        *out_match_count = cmd_count;

    return 2;
}

static void shell_cmd_exec_path(theshell_core_t* core, const char* cwd, const char* command, char* arg_line)
{
    if (!core || !cwd || !command || command[0] == '\0')
        return;

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, command, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "exec: invalid path\n");
        return;
    }

    char* argv_exec[SHELL_EXEC_MAX_ARGS + 1U];
    size_t argc_exec = 0U;
    argv_exec[argc_exec++] = resolved;

    if (arg_line && arg_line[0] != '\0')
    {
        char* parsed_args[SHELL_EXEC_MAX_ARGS];
        size_t parsed_count = shell_split_words(arg_line, parsed_args, SHELL_EXEC_MAX_ARGS - 1U);
        for (size_t i = 0; i < parsed_count && argc_exec < SHELL_EXEC_MAX_ARGS; i++)
            argv_exec[argc_exec++] = parsed_args[i];
    }
    argv_exec[argc_exec] = NULL;

    int fork_rc = fork();
    if (fork_rc < 0)
    {
        shell_output_printf(core, "exec: fork failed (rc=%d)\n", fork_rc);
        return;
    }

    if (fork_rc == 0)
    {
        int rc = execv(resolved, argv_exec);
        char errbuf[256];
        int errlen = snprintf(errbuf, sizeof(errbuf), "exec: cannot run '%s' (rc=%d)\n", resolved, rc);
        if (errlen > 0)
        {
            size_t written = (size_t) errlen;
            if (written >= sizeof(errbuf))
                written = sizeof(errbuf) - 1U;
            (void) write(STDERR_FILENO, errbuf, written);
        }
        _exit(127);
    }

    bool capture_enabled = (sys_console_route_set_sid((uint32_t) fork_rc,
                                                       SYS_CONSOLE_ROUTE_FLAG_CAPTURE) == 0);

    int wait_status = 0;
    int wait_rc = 0;
    for (;;)
    {
        wait_rc = waitpid(fork_rc, &wait_status, WNOHANG);
        if (wait_rc < 0)
        {
            shell_output_printf(core, "exec: waitpid failed for pid=%d\n", fork_rc);
            return;
        }

        if (capture_enabled)
            shell_drain_console_capture_sid(core, (uint32_t) fork_rc);

        if (wait_rc == 0)
        {
            (void) sys_yield();
            continue;
        }

        break;
    }

    if (capture_enabled)
    {
        shell_drain_console_capture_sid(core, (uint32_t) fork_rc);
        (void) sys_console_route_set_sid((uint32_t) fork_rc, 0U);
    }

    if (WIFSIGNALED(wait_status))
        shell_output_printf(core, "exec: pid=%d killed by %s\n", fork_rc, shell_signal_name(WTERMSIG(wait_status)));
    else if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) != 0)
        shell_output_printf(core, "exec: pid=%d exited status=%d\n", fork_rc, WEXITSTATUS(wait_status));
}

static void shell_cmd_pwd(theshell_core_t* core, const char* cwd)
{
    shell_output_printf(core, "%s\n", cwd);
}

static void shell_cmd_cd(theshell_core_t* core, char* cwd, const char* arg)
{
    const char* target = (arg && arg[0] != '\0') ? arg : "/";
    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, target, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "cd: invalid path\n");
        return;
    }

    if (!shell_path_is_dir(resolved))
    {
        shell_output_printf(core, "cd: no such directory: %s\n", resolved);
        return;
    }

    strcpy(cwd, resolved);
}

static void shell_cmd_ls(theshell_core_t* core, const char* cwd, const char* arg)
{
    char resolved[SHELL_PATH_MAX];
    const char* target = cwd;
    if (arg && arg[0] != '\0')
    {
        if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
        {
            shell_output_cstr(core, "ls: invalid path\n");
            return;
        }

        target = resolved;
    }

    if (shell_path_is_dir(target))
    {
        DIR* dir = opendir(target);
        if (!dir)
        {
            shell_output_printf(core, "ls: cannot access '%s'\n", target);
            return;
        }

        struct dirent* ent = NULL;
        while ((ent = readdir(dir)) != NULL)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            shell_output_printf(core, "%s%s\n", ent->d_name, ent->d_type == DT_DIR ? "/" : "");
        }

        (void) closedir(dir);
        return;
    }

    if (!shell_path_exists(target))
    {
        shell_output_printf(core, "ls: cannot access '%s'\n", target);
        return;
    }

    const char* leaf = NULL;
    for (const char* p = target; *p != '\0'; p++)
    {
        if (*p == '/')
            leaf = p;
    }

    if (!leaf || leaf[1] == '\0')
        shell_output_printf(core, "%s\n", target);
    else
        shell_output_printf(core, "%s\n", leaf + 1);
}

static void shell_cmd_cat(theshell_core_t* core, const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        shell_output_cstr(core, "cat: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "cat: invalid path\n");
        return;
    }

    static uint8_t buffer[SHELL_CAT_MAX];
    size_t out_size = 0;
    if (!shell_read_file(resolved, buffer, sizeof(buffer) - 1U, &out_size))
    {
        shell_output_printf(core, "cat: cannot read '%s'\n", resolved);
        return;
    }

    buffer[out_size] = '\0';
    if (out_size > 0U)
        shell_output_raw(core, (const char*) buffer, out_size);
    if (out_size == 0U || buffer[out_size - 1U] != '\n')
        shell_output_cstr(core, "\n");
}

static void shell_cmd_touch(theshell_core_t* core, const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        shell_output_cstr(core, "touch: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "touch: invalid path\n");
        return;
    }

    int fd = open(resolved, O_WRONLY | O_CREAT);
    if (fd < 0)
    {
        shell_output_printf(core, "touch: cannot touch '%s'\n", resolved);
        return;
    }

    if (close(fd) != 0)
        shell_output_printf(core, "touch: close failed for '%s'\n", resolved);
}

static void shell_cmd_mkdir(theshell_core_t* core, const char* cwd, const char* arg)
{
    if (!arg || arg[0] == '\0')
    {
        shell_output_cstr(core, "mkdir: missing operand\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, arg, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "mkdir: invalid path\n");
        return;
    }

    if (mkdir(resolved, 0755U) != 0)
        shell_output_printf(core, "mkdir: cannot create directory '%s'\n", resolved);
}

static void shell_cmd_echo(theshell_core_t* core, const char* cwd, char* arg)
{
    if (!arg)
    {
        shell_output_cstr(core, "\n");
        return;
    }

    char* trimmed_arg = shell_trim(arg);
    if (!trimmed_arg || trimmed_arg[0] == '\0')
    {
        shell_output_cstr(core, "\n");
        return;
    }

    char* pipe = shell_find_char(trimmed_arg, '|');
    if (!pipe)
    {
        shell_output_printf(core, "%s\n", trimmed_arg);
        return;
    }

    *pipe = '\0';
    char* text = shell_trim(trimmed_arg);
    char* target = shell_trim(pipe + 1U);
    if (!target || target[0] == '\0')
    {
        shell_output_cstr(core, "echo: missing target path after '|'\n");
        return;
    }

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, target, resolved, sizeof(resolved)))
    {
        shell_output_cstr(core, "echo: invalid target path\n");
        return;
    }

    int fd = open(resolved, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        shell_output_printf(core, "echo: cannot open '%s'\n", resolved);
        return;
    }

    const char* out_text = text ? text : "";
    size_t text_len = strlen(out_text);
    if (text_len > 0U && write(fd, out_text, text_len) != (int) text_len)
    {
        shell_output_printf(core, "echo: write failed on '%s'\n", resolved);
        (void) close(fd);
        return;
    }

    const char newline = '\n';
    if (write(fd, &newline, 1U) != 1)
    {
        shell_output_printf(core, "echo: write failed on '%s'\n", resolved);
        (void) close(fd);
        return;
    }

    if (close(fd) != 0)
        shell_output_printf(core, "echo: close failed on '%s'\n", resolved);
}

static void shell_cmd_clear(theshell_core_t* core)
{
    if (!core)
        return;

    if (core->io.clear)
    {
        core->io.clear(core->io.opaque);
        return;
    }

    (void) clear_screen();
}

static void shell_print_help(theshell_core_t* core)
{
    shell_output_cstr(core, "Commands:\n");
    shell_output_cstr(core, "  pwd\n");
    shell_output_cstr(core, "  cd <path>\n");
    shell_output_cstr(core, "  ls [path]\n");
    shell_output_cstr(core, "  cat <path>\n");
    shell_output_cstr(core, "  touch <path>\n");
    shell_output_cstr(core, "  mkdir <path>\n");
    shell_output_cstr(core, "  echo <text> | <path>\n");
    shell_output_cstr(core, "  clear\n");
    shell_output_cstr(core, "  <binary> (ex: TheTest or test)\n");
    shell_output_cstr(core, "  <alias> (ex: doom -> /bin/embeddedDOOM)\n");
    shell_output_cstr(core, "  <path/to/binary> (ex: /bin/TheTest or /drv/TheDHCPd)\n");
    shell_output_cstr(core, "  help\n");
    shell_output_cstr(core, "  exit\n");
}

void theshell_core_init(theshell_core_t* core, const theshell_io_t* io)
{
    if (!core)
        return;

    memset(core, 0, sizeof(*core));
    strcpy(core->cwd, "/");
    if (io)
        core->io = *io;
}

const char* theshell_core_cwd(const theshell_core_t* core)
{
    if (!core || core->cwd[0] == '\0')
        return "/";

    return core->cwd;
}

int theshell_core_execute_line(theshell_core_t* core, const char* line, bool* out_should_exit)
{
    if (!core || !line)
    {
        errno = EINVAL;
        return -1;
    }

    if (out_should_exit)
        *out_should_exit = false;

    char command_line[SHELL_LINE_MAX];
    size_t line_len = strlen(line);
    if (line_len >= sizeof(command_line))
        line_len = sizeof(command_line) - 1U;
    memcpy(command_line, line, line_len);
    command_line[line_len] = '\0';

    char* command = shell_trim(command_line);
    if (!command || command[0] == '\0')
        return 0;

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
        shell_cmd_pwd(core, core->cwd);
    else if (strcmp(command, "cd") == 0)
        shell_cmd_cd(core, core->cwd, arg);
    else if (strcmp(command, "ls") == 0)
        shell_cmd_ls(core, core->cwd, arg);
    else if (strcmp(command, "cat") == 0)
        shell_cmd_cat(core, core->cwd, arg);
    else if (strcmp(command, "touch") == 0)
        shell_cmd_touch(core, core->cwd, arg);
    else if (strcmp(command, "mkdir") == 0)
        shell_cmd_mkdir(core, core->cwd, arg);
    else if (strcmp(command, "echo") == 0)
        shell_cmd_echo(core, core->cwd, arg);
    else if (strcmp(command, "clear") == 0)
        shell_cmd_clear(core);
    else if (strcmp(command, "help") == 0)
        shell_print_help(core);
    else if (strcmp(command, "exit") == 0)
    {
        shell_output_cstr(core, "Bye !\n");
        if (out_should_exit)
            *out_should_exit = true;
    }
    else
    {
        char exec_command[SHELL_PATH_MAX];
        if (shell_resolve_exec_command(core->cwd, command, exec_command, sizeof(exec_command)))
            shell_cmd_exec_path(core, core->cwd, exec_command, arg);
        else
            shell_output_printf(core, "unknown command: %s\n", command);
    }

    return 0;
}
