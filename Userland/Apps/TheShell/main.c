#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <syscall.h>

#define SHELL_LINE_MAX           256U
#define SHELL_PATH_MAX           256U
#define SHELL_CAT_MAX            4096U
#define SHELL_MAX_COMPONENTS     32U
#define SHELL_MAX_COMPONENT_LEN  255U
#define SHELL_EXEC_MAX_ARGS      32U
#define SHELL_HISTORY_MAX        64U
#define SHELL_HISTORY_PATH       "/.TheShellHistory"

static const char* shell_known_binaries[] =
{
    "TheApp",
    "TheShell",
    "TheTest",
    "ThePowerManager",
    "TheMicroPython",
    "MicroPython"
};

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

    int fd = sys_open(path, SYS_OPEN_READ);
    if (fd < 0)
        return false;

    (void) sys_close(fd);
    return true;
}

static bool shell_build_the_alias_path(const char* command, char* out, size_t out_size)
{
    if (!command || !out || out_size < 7U)
        return false;

    const char* base = command;
    if (strncasecmp(command, "the", 3U) == 0 && command[3] != '\0')
    {
        base = command + 3U;
    }

    if (base[0] == '\0')
        return false;

    size_t pos = 0;
    const char prefix[] = "/bin/The";
    const size_t prefix_len = sizeof(prefix) - 1U;
    if (prefix_len + 2U > out_size)
        return false;

    memcpy(out + pos, prefix, prefix_len);
    pos += prefix_len;

    for (size_t i = 0; base[i] != '\0'; i++)
    {
        char c = (char) tolower((unsigned char) base[i]);
        if (i == 0U)
            c = (char) toupper((unsigned char) c);

        if (pos + 1U >= out_size)
            return false;

        out[pos++] = c;
    }

    out[pos] = '\0';
    return true;
}

static bool shell_command_key(const char* command, char* out, size_t out_size)
{
    if (!command || !out || out_size < 2U)
        return false;

    const char* base = command;
    if (strncasecmp(command, "the", 3U) == 0 && command[3] != '\0')
        base = command + 3U;

    if (base[0] == '\0')
        return false;

    size_t pos = 0;
    while (base[pos] != '\0')
    {
        if (pos + 1U >= out_size)
            return false;
        out[pos] = (char) tolower((unsigned char) base[pos]);
        pos++;
    }

    out[pos] = '\0';
    return true;
}

static bool shell_build_known_alias_path(const char* command, char* out, size_t out_size)
{
    if (!command || !out || out_size < 8U)
        return false;

    char command_key[SHELL_PATH_MAX];
    if (!shell_command_key(command, command_key, sizeof(command_key)))
        return false;

    for (size_t i = 0; i < sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0]); i++)
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

static bool shell_resolve_exec_command(const char* cwd, const char* command, char* out, size_t out_size)
{
    if (!cwd || !command || !out || out_size == 0U || command[0] == '\0')
        return false;

    if (shell_command_is_path(command))
        return shell_resolve_path(cwd, command, out, out_size);

    int direct_len = snprintf(out, out_size, "/bin/%s", command);
    if (direct_len > 0 && (size_t) direct_len < out_size && shell_path_exists(out))
        return true;

    char known_alias_path[SHELL_PATH_MAX];
    if (shell_build_known_alias_path(command, known_alias_path, sizeof(known_alias_path)) &&
        shell_path_exists(known_alias_path))
    {
        size_t len = strlen(known_alias_path);
        if (len + 1U > out_size)
            return false;
        memcpy(out, known_alias_path, len + 1U);
        return true;
    }

    char alias_path[SHELL_PATH_MAX];
    if (!shell_build_the_alias_path(command, alias_path, sizeof(alias_path)))
        return false;
    if (!shell_path_exists(alias_path))
        return false;

    size_t alias_len = strlen(alias_path);
    if (alias_len + 1U > out_size)
        return false;

    memcpy(out, alias_path, alias_len + 1U);
    return true;
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

typedef struct shell_history
{
    char entries[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
    size_t count;
    size_t browse_index;
    bool browsing;
    char draft[SHELL_LINE_MAX];
} shell_history_t;

static bool shell_read_file(const char* path, uint8_t* buf, size_t buf_size, size_t* out_size)
{
    if (!path || !buf || buf_size == 0U || !out_size)
        return false;

    int fd = sys_open(path, SYS_OPEN_READ);
    if (fd < 0)
        return false;

    size_t total = 0U;
    while (total < buf_size)
    {
        int rc = sys_read(fd, buf + total, buf_size - total);
        if (rc < 0)
        {
            (void) sys_close(fd);
            return false;
        }
        if (rc == 0)
            break;
        total += (size_t) rc;
    }

    (void) sys_close(fd);
    *out_size = total;
    return true;
}

static bool shell_starts_with_nocase(const char* text, const char* prefix)
{
    if (!text || !prefix)
        return false;

    size_t i = 0;
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
            is_dir = (fs_is_dir(abs_path) == 1);
        }

        if (strcmp(dir_part, ".") == 0)
        {
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "%s", ent->d_name);
        }
        else if (strcmp(dir_part, "/") == 0)
        {
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "/%s", ent->d_name);
        }
        else
        {
            snprintf(out_matches[*out_count], SHELL_PATH_MAX, "%s/%s", dir_part, ent->d_name);
        }

        out_is_dir[*out_count] = is_dir;
        (*out_count)++;
    }

    (void) closedir(dir);
}

static void shell_history_add(shell_history_t* history, const char* line)
{
    if (!history || !line || line[0] == '\0')
        return;

    if (history->count > 0U &&
        strcmp(history->entries[history->count - 1U], line) == 0)
    {
        history->browse_index = history->count;
        history->browsing = false;
        history->draft[0] = '\0';
        return;
    }

    if (history->count == SHELL_HISTORY_MAX)
    {
        for (size_t i = 1U; i < SHELL_HISTORY_MAX; i++)
            strcpy(history->entries[i - 1U], history->entries[i]);
        history->count = SHELL_HISTORY_MAX - 1U;
    }

    strcpy(history->entries[history->count], line);
    history->count++;
    history->browse_index = history->count;
    history->browsing = false;
    history->draft[0] = '\0';
}

static void shell_history_load(shell_history_t* history)
{
    if (!history)
        return;

    memset(history, 0, sizeof(*history));

    static uint8_t file_buf[SHELL_HISTORY_MAX * SHELL_LINE_MAX];
    size_t out_size = 0;
    if (!shell_read_file(SHELL_HISTORY_PATH, file_buf, sizeof(file_buf) - 1U, &out_size))
    {
        history->browse_index = 0U;
        return;
    }
    file_buf[out_size] = '\0';

    char* cursor = (char*) file_buf;
    while (*cursor != '\0')
    {
        char* line = cursor;
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
        {
            *cursor = '\0';
            cursor++;
        }

        char* trimmed = shell_trim(line);
        if (!trimmed || trimmed[0] == '\0')
            continue;

        shell_history_add(history, trimmed);
    }
}

static void shell_history_save_append(const char* line)
{
    if (!line || line[0] == '\0')
        return;

    int fd = sys_open(SHELL_HISTORY_PATH, SYS_OPEN_CREATE | SYS_OPEN_WRITE);
    if (fd < 0)
        return;

    int64_t end = sys_lseek(fd, 0, SYS_SEEK_END);
    if (end >= 0)
    {
        size_t len = strlen(line);
        if (len > 0U)
            (void) sys_write(fd, line, len);
        (void) sys_write(fd, "\n", 1U);
    }

    (void) sys_close(fd);
}

static void shell_redraw_input(const char* prompt,
                               const char* line,
                               size_t length,
                               size_t cursor,
                               size_t* inout_prev_len)
{
    if (!prompt || !line || !inout_prev_len)
        return;

    size_t prev_len = *inout_prev_len;
    size_t clear_count = (prev_len > length) ? (prev_len - length) : 0U;

    putc('\r');
    (void) sys_console_write(prompt, strlen(prompt));
    if (length > 0U)
        (void) sys_console_write(line, length);

    for (size_t i = 0; i < clear_count; i++)
        putc(' ');

    size_t tail = length + clear_count;
    while (tail > cursor)
    {
        putc('\b');
        tail--;
    }

    *inout_prev_len = length;
}

static size_t shell_collect_command_matches(const char* prefix,
                                            const char* matches[],
                                            size_t max_matches)
{
    if (!prefix || !matches || max_matches == 0U)
        return 0U;

    static char alias_names[sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0])][SHELL_PATH_MAX];
    size_t count = 0U;

    #define ADD_MATCH_UNIQUE(candidate) \
        do { \
            const char* _c = (candidate); \
            bool _dup = false; \
            size_t _safe_count = (count < max_matches) ? count : max_matches; \
            for (size_t _j = 0; _j < _safe_count; _j++) { \
                if (strcasecmp(matches[_j], _c) == 0) { \
                    _dup = true; \
                    break; \
                } \
            } \
            if (!_dup) { \
                if (count < max_matches) \
                    matches[count] = _c; \
                count++; \
            } \
        } while (0)

    for (size_t i = 0; i < sizeof(shell_builtin_commands) / sizeof(shell_builtin_commands[0]); i++)
    {
        if (shell_starts_with_nocase(shell_builtin_commands[i], prefix))
            ADD_MATCH_UNIQUE(shell_builtin_commands[i]);
    }

    for (size_t i = 0; i < sizeof(shell_known_binaries) / sizeof(shell_known_binaries[0]); i++)
    {
        if (shell_starts_with_nocase(shell_known_binaries[i], prefix))
            ADD_MATCH_UNIQUE(shell_known_binaries[i]);

        if (shell_command_key(shell_known_binaries[i], alias_names[i], sizeof(alias_names[i])) &&
            shell_starts_with_nocase(alias_names[i], prefix))
        {
            ADD_MATCH_UNIQUE(alias_names[i]);
        }
    }

    #undef ADD_MATCH_UNIQUE

    return count;
}

static void shell_input_history_prev(shell_history_t* history,
                                     char* line,
                                     size_t* inout_len,
                                     size_t* inout_cursor)
{
    if (!history || !line || !inout_len || !inout_cursor || history->count == 0U)
        return;

    if (!history->browsing)
    {
        memcpy(history->draft, line, *inout_len);
        history->draft[*inout_len] = '\0';
        history->browse_index = history->count;
        history->browsing = true;
    }

    if (history->browse_index == 0U)
        return;

    history->browse_index--;
    strcpy(line, history->entries[history->browse_index]);
    *inout_len = strlen(line);
    *inout_cursor = *inout_len;
}

static void shell_input_history_next(shell_history_t* history,
                                     char* line,
                                     size_t* inout_len,
                                     size_t* inout_cursor)
{
    if (!history || !line || !inout_len || !inout_cursor || !history->browsing)
        return;

    if (history->browse_index + 1U < history->count)
    {
        history->browse_index++;
        strcpy(line, history->entries[history->browse_index]);
    }
    else
    {
        history->browse_index = history->count;
        strcpy(line, history->draft);
        history->browsing = false;
    }

    *inout_len = strlen(line);
    *inout_cursor = *inout_len;
}

static void shell_input_autocomplete(const char* prompt,
                                     const char* cwd,
                                     char* line,
                                     size_t* inout_len,
                                     size_t* inout_cursor,
                                     size_t* inout_prev_len)
{
    if (!prompt || !cwd || !line || !inout_len || !inout_cursor || !inout_prev_len)
        return;

    size_t len = *inout_len;
    size_t cursor = *inout_cursor;
    if (cursor == 0U)
        return;

    size_t token_start = cursor;
    while (token_start > 0U)
    {
        char c = line[token_start - 1U];
        if (c == ' ' || c == '\t')
            break;
        token_start--;
    }

    bool is_first_token = (token_start == 0U);

    for (size_t i = token_start; i < cursor; i++)
    {
        if (line[i] == '/')
        {
            is_first_token = false;
            break;
        }
    }

    char prefix[SHELL_LINE_MAX];
    memcpy(prefix, line + token_start, cursor - token_start);
    prefix[cursor - token_start] = '\0';

    if (!is_first_token)
    {
        char matches[64][SHELL_PATH_MAX];
        bool is_dir[64];
        size_t match_count = 0U;
        shell_tab_complete_argument_path(cwd, prefix, matches, is_dir, 64U, &match_count);
        if (match_count == 0U)
            return;

        if (match_count == 1U)
        {
            char replacement[SHELL_PATH_MAX + 2U];
            size_t repl_len = strlen(matches[0]);
            if (repl_len + 2U > sizeof(replacement))
                return;
            memcpy(replacement, matches[0], repl_len + 1U);
            if (is_dir[0] && (repl_len == 0U || replacement[repl_len - 1U] != '/'))
            {
                replacement[repl_len++] = '/';
                replacement[repl_len] = '\0';
            }

            size_t suffix_len = len - cursor;
            if (token_start + repl_len + suffix_len >= SHELL_LINE_MAX)
                return;

            memcpy(line + token_start, replacement, repl_len);
            if (suffix_len > 0U)
                memmove(line + token_start + repl_len, line + cursor, suffix_len);
            len = token_start + repl_len + suffix_len;
            line[len] = '\0';
            cursor = token_start + repl_len;
            *inout_len = len;
            *inout_cursor = cursor;
        }
        else
        {
            putc('\n');
            for (size_t i = 0; i < match_count; i++)
            {
                printf("%s%s%s",
                       matches[i],
                       is_dir[i] ? "/" : "",
                       (i + 1U < match_count) ? "  " : "\n");
            }
            *inout_prev_len = 0U;
            shell_redraw_input(prompt, line, len, cursor, inout_prev_len);
        }
        return;
    }

    const char* matches[32];
    size_t match_count = shell_collect_command_matches(prefix, matches, 32U);
    if (match_count == 0U)
        return;

    if (match_count == 1U)
    {
        const char* chosen = matches[0];
        size_t chosen_len = strlen(chosen);
        if (chosen_len > SHELL_LINE_MAX - 1U)
            chosen_len = SHELL_LINE_MAX - 1U;

        size_t suffix_len = len - cursor;
        if (chosen_len + suffix_len >= SHELL_LINE_MAX)
            return;

        memcpy(line + token_start, chosen, chosen_len);
        if (suffix_len > 0U)
            memmove(line + token_start + chosen_len, line + cursor, suffix_len);

        len = token_start + chosen_len + suffix_len;
        line[len] = '\0';
        cursor = token_start + chosen_len;
        *inout_len = len;
        *inout_cursor = cursor;
        return;
    }

    putc('\n');
    for (size_t i = 0; i < match_count; i++)
        printf("%s%s", matches[i], (i + 1U < match_count) ? "  " : "\n");

    *inout_prev_len = 0U;
    shell_redraw_input(prompt, line, len, cursor, inout_prev_len);
}

static bool shell_read_line(const char* prompt,
                            const char* cwd,
                            char* out_line,
                            size_t out_size,
                            shell_history_t* history)
{
    if (!prompt || !cwd || !out_line || out_size < 2U)
        return false;

    size_t len = 0U;
    size_t cursor = 0U;
    out_line[0] = '\0';
    size_t prev_len = 0U;

    if (history)
    {
        history->browsing = false;
        history->browse_index = history->count;
        history->draft[0] = '\0';
    }

    for (;;)
    {
        int key = getchar();
        if (key == EOF)
            continue;

        if (key == '\r' || key == '\n')
        {
            putc('\n');
            out_line[len] = '\0';
            return true;
        }

        if (key == '\b')
        {
            if (cursor > 0U)
            {
                memmove(out_line + cursor - 1U, out_line + cursor, len - cursor);
                cursor--;
                len--;
                out_line[len] = '\0';
            }
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == STDIO_KEY_DELETE)
        {
            if (cursor < len)
            {
                memmove(out_line + cursor, out_line + cursor + 1U, len - cursor - 1U);
                len--;
                out_line[len] = '\0';
            }
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == STDIO_KEY_LEFT)
        {
            if (cursor > 0U)
                cursor--;
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == STDIO_KEY_RIGHT)
        {
            if (cursor < len)
                cursor++;
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == STDIO_KEY_UP)
        {
            if (history)
                shell_input_history_prev(history, out_line, &len, &cursor);
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == STDIO_KEY_DOWN)
        {
            if (history)
                shell_input_history_next(history, out_line, &len, &cursor);
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key == '\t')
        {
            shell_input_autocomplete(prompt, cwd, out_line, &len, &cursor, &prev_len);
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
            continue;
        }

        if (key >= 32 && key <= 126)
        {
            if (len + 1U >= out_size)
                continue;

            if (cursor == len)
            {
                out_line[len++] = (char) key;
                out_line[len] = '\0';
                cursor = len;
            }
            else
            {
                memmove(out_line + cursor + 1U, out_line + cursor, len - cursor);
                out_line[cursor] = (char) key;
                len++;
                cursor++;
                out_line[len] = '\0';
            }

            if (history)
            {
                history->browsing = false;
                history->browse_index = history->count;
            }
            shell_redraw_input(prompt, out_line, len, cursor, &prev_len);
        }
    }
}

static void shell_cmd_exec_path(const char* cwd, const char* command, char* arg_line)
{
    if (!cwd || !command || command[0] == '\0')
        return;

    char resolved[SHELL_PATH_MAX];
    if (!shell_resolve_path(cwd, command, resolved, sizeof(resolved)))
    {
        printf("exec: invalid path\n");
        return;
    }

    const char* argv_exec[SHELL_EXEC_MAX_ARGS + 1U];
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

    int fork_rc = sys_fork();
    if (fork_rc < 0)
    {
        printf("exec: fork failed (rc=%d)\n", fork_rc);
        return;
    }

    if (fork_rc == 0)
    {
        const char* const envp[] = { NULL };
        int rc = sys_execve(resolved, argv_exec, envp);
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

    if (fs_is_dir(target) == 1)
    {
        DIR* dir = opendir(target);
        if (!dir)
        {
            printf("ls: cannot access '%s'\n", target);
            return;
        }

        struct dirent* ent = NULL;
        while ((ent = readdir(dir)) != NULL)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            printf("%s%s\n", ent->d_name, ent->d_type == DT_DIR ? "/" : "");
        }

        (void) closedir(dir);
        return;
    }

    if (!shell_path_exists(target))
    {
        printf("ls: cannot access '%s'\n", target);
        return;
    }

    const char* leaf = NULL;
    for (const char* p = target; *p != '\0'; p++)
    {
        if (*p == '/')
            leaf = p;
    }

    if (!leaf || leaf[1] == '\0')
        printf("%s\n", target);
    else
        printf("%s\n", leaf + 1);
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
    if (!shell_read_file(resolved, buffer, sizeof(buffer) - 1U, &out_size))
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
    if (text_len > 0U && sys_write(fd, out_text, text_len) != (int) text_len)
    {
        printf("echo: write failed on '%s'\n", resolved);
        (void) sys_close(fd);
        return;
    }

    const char newline = '\n';
    if (sys_write(fd, &newline, 1U) != 1)
    {
        printf("echo: write failed on '%s'\n", resolved);
        (void) sys_close(fd);
        return;
    }

    if (sys_close(fd) != 0)
        printf("echo: close failed on '%s'\n", resolved);
}

static void shell_cmd_clear(void)
{
    static const char clear_seq[] = "\x1b[2J\x1b[H";
    (void) sys_console_write(clear_seq, sizeof(clear_seq) - 1U);
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
    printf("  clear\n");
    printf("  <binary> (ex: TheTest ou test)\n");
    printf("  <path/to/binary> (ex: /bin/TheTest)\n");
    printf("  help\n");
    printf("  exit\n");
}

int main(int argc, char** argv, char** envp)
{
    (void) argc;
    (void) argv;
    (void) envp;

    if (keyboard_load_config("/system/keyboard.conf") != 0)
        printf("[TheShell] keyboard config unavailable, fallback=qwerty\n");

    char cwd[SHELL_PATH_MAX];
    strcpy(cwd, "/");
    shell_history_t history;
    shell_history_load(&history);

    printf("TheShell started.\n");
    printf("Type 'help' for commands.\n");

    for (;;)
    {
        char prompt[SHELL_PATH_MAX + 16U];
        int prompt_len = snprintf(prompt, sizeof(prompt), "TheShell:%s$ ", cwd);
        if (prompt_len <= 0 || (size_t) prompt_len >= sizeof(prompt))
            strcpy(prompt, "TheShell:/$ ");
        (void) sys_console_write(prompt, strlen(prompt));

        char line[SHELL_LINE_MAX];
        if (!shell_read_line(prompt, cwd, line, sizeof(line), &history))
            continue;

        char* command = shell_trim(line);
        if (!command || command[0] == '\0')
            continue;

        shell_history_add(&history, command);
        shell_history_save_append(command);

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
        else if (strcmp(command, "clear") == 0)
            shell_cmd_clear();
        else if (strcmp(command, "help") == 0)
            shell_print_help();
        else if (strcmp(command, "exit") == 0)
        {
            printf("Bye !\n");
            sys_exit(0);
        }
        else
        {
            char exec_command[SHELL_PATH_MAX];
            if (shell_resolve_exec_command(cwd, command, exec_command, sizeof(exec_command)))
                shell_cmd_exec_path(cwd, exec_command, arg);
            else
                printf("unknown command: %s\n", command);
        }
    }
}
