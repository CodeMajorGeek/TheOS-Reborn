#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <window.h>

#define SHELLGUI_LINES_MAX     64U
#define SHELLGUI_LINE_LEN      160U
#define SHELLGUI_VISIBLE_LINES 24U
#define SHELLGUI_REFRESH_MIN_TICKS 0ULL
#define SHELLGUI_EVENT_POLL_BACKOFF_TICKS 0ULL
#define SHELLGUI_CURSOR_BLINK_TICKS 125ULL

#define SHELLGUI_LOG(fmt, ...)                                                                                              \
    do                                                                                                                      \
    {                                                                                                                       \
        unsigned long long __tick = (unsigned long long) sys_tick_get();                                                   \
        printf("[TheShellGUI t=%llu] " fmt, __tick, ##__VA_ARGS__);                                                       \
        (void) fflush(stdout);                                                                                              \
    }                                                                                                                       \
    while (0)

typedef struct shellgui_state
{
    ws_client_t client;
    uint32_t window_id;
    pid_t shell_pid;
    bool shell_running;
    char lines[SHELLGUI_LINES_MAX][SHELLGUI_LINE_LEN];
    uint32_t line_count;
    uint32_t line_head;
    char stream_line[SHELLGUI_LINE_LEN];
    size_t stream_line_len;
    bool stream_pending_cr;
    bool ansi_escape;
    bool ansi_csi;
    uint32_t ansi_param;
    bool ansi_param_seen;
    bool text_dirty;
    uint64_t next_refresh_tick;
    uint64_t next_event_poll_tick;
    char last_sent_text[WS_WINDOW_BODY_TEXT_MAX + 1U];
    bool key_extended_prefix;
    bool key_down[256U];
    bool cursor_visible;
    uint64_t next_cursor_blink_tick;
} shellgui_state_t;

static void shellgui_copy_string_limit(char* out, size_t out_size, const char* in)
{
    if (!out || out_size == 0U)
        return;

    out[0] = '\0';
    if (!in || in[0] == '\0')
        return;

    size_t len = strlen(in);
    if (len >= out_size)
        len = out_size - 1U;
    memcpy(out, in, len);
    out[len] = '\0';
}

static void shellgui_buf_append(char* dst, size_t dst_size, const char* text)
{
    if (!dst || dst_size == 0U || !text)
        return;

    size_t used = strlen(dst);
    if (used >= dst_size - 1U)
        return;

    size_t add = strlen(text);
    if (add > (dst_size - 1U - used))
        add = dst_size - 1U - used;

    memcpy(dst + used, text, add);
    dst[used + add] = '\0';
}

static void shellgui_push_line(shellgui_state_t* state, const char* line)
{
    if (!state)
        return;

    uint32_t index = 0U;
    if (state->line_count < SHELLGUI_LINES_MAX)
    {
        index = (state->line_head + state->line_count) % SHELLGUI_LINES_MAX;
        state->line_count++;
    }
    else
    {
        index = state->line_head;
        state->line_head = (state->line_head + 1U) % SHELLGUI_LINES_MAX;
    }

    shellgui_copy_string_limit(state->lines[index], sizeof(state->lines[index]), line ? line : "");
}

static void shellgui_stream_flush(shellgui_state_t* state)
{
    if (!state || state->stream_line_len == 0U)
        return;

    state->stream_line[state->stream_line_len] = '\0';
    shellgui_push_line(state, state->stream_line);
    state->stream_line_len = 0U;
    state->stream_line[0] = '\0';
    state->stream_pending_cr = false;
}

static void shellgui_stream_clear(shellgui_state_t* state)
{
    if (!state)
        return;

    state->line_count = 0U;
    state->line_head = 0U;
    state->stream_line_len = 0U;
    state->stream_line[0] = '\0';
    state->stream_pending_cr = false;
}

static void shellgui_ansi_reset(shellgui_state_t* state)
{
    if (!state)
        return;

    state->ansi_escape = false;
    state->ansi_csi = false;
    state->ansi_param = 0U;
    state->ansi_param_seen = false;
}

static void shellgui_ansi_handle_csi(shellgui_state_t* state, char final_char)
{
    if (!state)
        return;

    uint32_t param = state->ansi_param_seen ? state->ansi_param : 0U;

    switch (final_char)
    {
        case 'J':
            if (param == 0U || param == 2U)
                shellgui_stream_clear(state);
            break;
        case 'H':
        case 'f':
            state->stream_line_len = 0U;
            state->stream_line[0] = '\0';
            state->stream_pending_cr = false;
            break;
        case 'K':
            state->stream_line_len = 0U;
            state->stream_line[0] = '\0';
            break;
        default:
            break;
    }
}

static void shellgui_stream_push_char(shellgui_state_t* state, char c)
{
    if (!state)
        return;

    if (state->ansi_escape)
    {
        if (!state->ansi_csi)
        {
            if (c == '[')
            {
                state->ansi_csi = true;
                state->ansi_param = 0U;
                state->ansi_param_seen = false;
                return;
            }

            shellgui_ansi_reset(state);
        }
        else
        {
            if (c >= '0' && c <= '9')
            {
                state->ansi_param = state->ansi_param * 10U + (uint32_t) (c - '0');
                state->ansi_param_seen = true;
                return;
            }
            if (c == ';')
            {
                state->ansi_param_seen = true;
                return;
            }
            /* Préfixe CSI privé ex. \x1b[?25h — ne pas réinitialiser ni laisser fuiter "?..." en clair. */
            if (c == '?')
                return;

            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                shellgui_ansi_handle_csi(state, c);
            shellgui_ansi_reset(state);
            return;
        }
    }

    if ((unsigned char) c == 0x1B)
    {
        state->ansi_escape = true;
        state->ansi_csi = false;
        state->ansi_param = 0U;
        state->ansi_param_seen = false;
        return;
    }

    if (c == '\r')
    {
        state->stream_pending_cr = true;
        return;
    }
    if (c == '\n')
    {
        shellgui_stream_flush(state);
        return;
    }
    if (state->stream_pending_cr)
    {
        state->stream_line_len = 0U;
        state->stream_line[0] = '\0';
        state->stream_pending_cr = false;
    }
    if (c == '\b' || c == 127)
    {
        if (state->stream_line_len > 0U)
        {
            state->stream_line_len--;
            state->stream_line[state->stream_line_len] = '\0';
        }
        return;
    }
    if ((unsigned char) c < 32U && c != '\t')
        return;

    if (state->stream_line_len + 1U >= sizeof(state->stream_line))
        return;

    if (c == '\t')
    {
        static const char tab_spaces[] = "    ";
        for (size_t i = 0U; i < sizeof(tab_spaces) - 1U; i++)
        {
            if (state->stream_line_len + 1U >= sizeof(state->stream_line))
                break;
            state->stream_line[state->stream_line_len++] = tab_spaces[i];
        }
        state->stream_line[state->stream_line_len] = '\0';
        return;
    }

    state->stream_line[state->stream_line_len++] = c;
    state->stream_line[state->stream_line_len] = '\0';
}

static void shellgui_compose_text(const shellgui_state_t* state, char* out_text, size_t out_size)
{
    if (!state || !out_text || out_size == 0U)
        return;

    out_text[0] = '\0';
    uint32_t history_budget = SHELLGUI_VISIBLE_LINES;
    if (state->stream_line_len > 0U && history_budget > 0U)
        history_budget--;

    uint32_t shown_history = state->line_count;
    if (shown_history > history_budget)
        shown_history = history_budget;

    uint32_t skip_history = state->line_count - shown_history;
    for (uint32_t i = 0U; i < shown_history; i++)
    {
        uint32_t idx = (state->line_head + skip_history + i) % SHELLGUI_LINES_MAX;
        shellgui_buf_append(out_text, out_size, state->lines[idx]);
        shellgui_buf_append(out_text, out_size, "\n");
    }

    if (state->stream_line_len > 0U)
        shellgui_buf_append(out_text, out_size, state->stream_line);

    if (state->cursor_visible)
        shellgui_buf_append(out_text, out_size, "_");
}

static bool shellgui_refresh_window(shellgui_state_t* state)
{
    if (!state || state->window_id == 0U)
        return false;

    char text[WS_WINDOW_BODY_TEXT_MAX + 1U];
    shellgui_compose_text(state, text, sizeof(text));
    if (strcmp(text, state->last_sent_text) == 0)
        return true;

    if (ws_client_set_window_text(&state->client, state->window_id, text) != 0)
        return false;

    shellgui_copy_string_limit(state->last_sent_text, sizeof(state->last_sent_text), text);
    return true;
}

static bool shellgui_launch_shell(shellgui_state_t* state)
{
    if (!state)
        return false;

    pid_t pid = fork();
    if (pid < 0)
    {
        SHELLGUI_LOG("fork failed errno=%d\n", errno);
        return false;
    }

    if (pid == 0)
    {
        char* const argv[] = {
            "TheShell",
            NULL
        };
        (void) execv("/bin/TheShell", argv);
        _exit(127);
    }

    if (sys_console_route_set_sid((uint32_t) pid,
                                 SYS_CONSOLE_ROUTE_FLAG_CAPTURE | SYS_CONSOLE_ROUTE_FLAG_PTY_INPUT) < 0)
    {
        SHELLGUI_LOG("console route set failed pid=%d errno=%d\n", (int) pid, errno);
        (void) kill(pid, SIGTERM);
        return false;
    }

    SHELLGUI_LOG("TheShell launched pid=%d (PTY)\n", (int) pid);
    state->shell_pid = pid;
    state->shell_running = true;
    return true;
}

static bool shellgui_pty_feed_key(shellgui_state_t* state, int key)
{
    if (!state || !state->shell_running || state->shell_pid <= 0 || key <= 0)
        return false;

    unsigned char b = (unsigned char) (key & 0xFF);
    return sys_console_route_input_write_sid((uint32_t) state->shell_pid, &b, 1U) >= 0;
}

static bool shellgui_scancode_is_fresh_press(shellgui_state_t* state, uint8_t raw_scancode)
{
    if (!state)
        return false;

    if (raw_scancode == 0xE0U)
    {
        state->key_extended_prefix = true;
        return false;
    }

    bool is_extended = state->key_extended_prefix;
    state->key_extended_prefix = false;

    bool is_break = (raw_scancode & 0x80U) != 0U;
    uint16_t slot = (uint16_t) (raw_scancode & 0x7FU);
    if (is_extended)
        slot |= 0x80U;

    if (is_break)
    {
        state->key_down[slot] = false;
        return false;
    }

    if (state->key_down[slot])
        return false;

    state->key_down[slot] = true;
    return true;
}

static bool shellgui_poll_capture(shellgui_state_t* state)
{
    if (!state || !state->shell_running || state->shell_pid <= 0)
        return false;

    bool changed = false;
    char capture[512];
    for (;;)
    {
        int rc = sys_console_route_read_sid((uint32_t) state->shell_pid, capture, sizeof(capture));
        if (rc <= 0)
            break;

        for (int i = 0; i < rc; i++)
            shellgui_stream_push_char(state, capture[i]);
        changed = true;
    }
    return changed;
}

static bool shellgui_poll_child_status(shellgui_state_t* state)
{
    if (!state || !state->shell_running || state->shell_pid <= 0)
        return false;

    int status = 0;
    int rc = waitpid(state->shell_pid, &status, WNOHANG);
    if (rc != state->shell_pid)
        return false;

    state->shell_running = false;
    shellgui_stream_flush(state);
    if (WIFSIGNALED(status))
        shellgui_push_line(state, "[TheShell terminated by signal]");
    else if (WIFEXITED(status))
        shellgui_push_line(state, "[TheShell exited]");

    (void) sys_console_route_set_sid((uint32_t) state->shell_pid, 0U);
    return true;
}

static void shellgui_cleanup(shellgui_state_t* state)
{
    if (!state)
        return;

    if (state->shell_running && state->shell_pid > 0)
        (void) kill(state->shell_pid, SIGTERM);
    if (state->shell_pid > 0)
    {
        (void) waitpid(state->shell_pid, NULL, WNOHANG);
        (void) sys_console_route_set_sid((uint32_t) state->shell_pid, 0U);
    }

    if (state->window_id != 0U)
        (void) ws_client_destroy_window(&state->client, state->window_id);
    ws_client_disconnect(&state->client);
}

int main(void)
{
    shellgui_state_t state;
    memset(&state, 0, sizeof(state));
    state.shell_pid = -1;

    if (keyboard_load_config("/system/keyboard.conf") != 0)
        SHELLGUI_LOG("keyboard config unavailable, using defaults for PTY decode\n");

    if (ws_client_connect(&state.client, WS_CLIENT_ROLE_SHELL_GUI) < 0)
    {
        int e = errno;
        SHELLGUI_LOG("ws_client_connect failed errno=%d\n", e);
        return 1;
    }

    ws_window_desc_t window_desc = {
        .x = 80,
        .y = 72,
        .width = 700U,
        .height = 420U,
        .color = 0x001B2532U,
        .border_color = 0x00435E7DU,
        .titlebar_color = 0x00344D69U,
        .visible = true,
        .frame_controls = true,
        .title = "TheShellGUI"
    };

    if (ws_client_create_window(&state.client, &window_desc, &state.window_id) < 0)
    {
        int e = errno;
        SHELLGUI_LOG("create_window failed errno=%d\n", e);
        ws_client_disconnect(&state.client);
        return 1;
    }
    SHELLGUI_LOG("window created id=%u\n", (unsigned int) state.window_id);

    if (!shellgui_launch_shell(&state))
        shellgui_push_line(&state, "[failed to start TheShell]");
    state.last_sent_text[0] = '\0';
    state.cursor_visible = true;
    state.next_cursor_blink_tick = sys_tick_get() + SHELLGUI_CURSOR_BLINK_TICKS;
    (void) shellgui_refresh_window(&state);
    state.text_dirty = false;
    state.next_refresh_tick = sys_tick_get() + SHELLGUI_REFRESH_MIN_TICKS;
    state.next_event_poll_tick = 0ULL;

    bool should_exit = false;
    while (!should_exit)
    {
        bool changed = false;

        if (shellgui_poll_capture(&state))
            changed = true;
        if (shellgui_poll_child_status(&state))
        {
            changed = true;
            should_exit = true;
        }

        uint64_t now_tick = sys_tick_get();
        if (now_tick >= state.next_event_poll_tick)
        {
            for (;;)
            {
                ws_event_t event;
                if (ws_client_poll_event(&state.client, &event) < 0)
                {
                    int poll_errno = errno;
                    errno = poll_errno;
                    if (poll_errno == EAGAIN)
                    {
                        state.next_event_poll_tick = now_tick + SHELLGUI_EVENT_POLL_BACKOFF_TICKS;
                        break;
                    }

                    SHELLGUI_LOG("poll_event failed errno=%d cached=%u connected=%u win=%u\n",
                                 poll_errno,
                                 state.client.poll_event_cached ? 1U : 0U,
                                 state.client.connected ? 1U : 0U,
                                 (unsigned int) state.window_id);
                    should_exit = true;
                    break;
                }

                if (event.type == WS_EVENT_CLOSE && event.window_id == state.window_id)
                {
                    SHELLGUI_LOG("window close event win=%u shell_pid=%d running=%u\n",
                                 (unsigned int) event.window_id,
                                 (int) state.shell_pid,
                                 state.shell_running ? 1U : 0U);
                    should_exit = true;
                    break;
                }
                /* La file d'événements est par client : seul le client focalisé reçoit des KEY.
                 * Ne pas filtrer par window_id : une divergence d'ID bloquait tout envoi au PTY. */
                if (event.type == WS_EVENT_KEY)
                {
                    uint8_t raw_scancode = (uint8_t) ((unsigned int) event.key & 0xFFU);
                    bool fresh_press = shellgui_scancode_is_fresh_press(&state, raw_scancode);

                    int decoded = 0;
                    int dec_rc = keyboard_decode_scancode(raw_scancode, &decoded);
                    if (fresh_press && dec_rc > 0 && decoded > 0)
                        (void) shellgui_pty_feed_key(&state, decoded);
                }
            }
        }

        if (changed)
            state.text_dirty = true;

        now_tick = sys_tick_get();
        if (now_tick >= state.next_cursor_blink_tick)
        {
            state.cursor_visible = !state.cursor_visible;
            state.text_dirty = true;
            state.next_cursor_blink_tick = now_tick + SHELLGUI_CURSOR_BLINK_TICKS;
        }

        now_tick = sys_tick_get();
        if (state.text_dirty &&
            (should_exit || now_tick >= state.next_refresh_tick))
        {
            if (shellgui_refresh_window(&state))
            {
                state.text_dirty = false;
                state.next_refresh_tick = now_tick + SHELLGUI_REFRESH_MIN_TICKS;
            }
            else
            {
                SHELLGUI_LOG("refresh window failed errno=%d cached=%u dirty=%u\n",
                             errno,
                             state.client.poll_event_cached ? 1U : 0U,
                             state.text_dirty ? 1U : 0U);
            }
        }

        /* usleep() LibC arrondit à ≥1 ms (unistd.c) : chaque tour de boucle était bridé ~1000 Hz. */
        (void) sched_yield();
    }

    shellgui_cleanup(&state);
    SHELLGUI_LOG("exit\n");
    return 0;
}
