#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <window.h>

#define SHELLGUI_FRAME_TIME_US 8000U
#define SHELLGUI_IDLE_SLEEP_US 2000U
#define SHELLGUI_LINES_MAX     64U
#define SHELLGUI_LINE_LEN      160U
#define SHELLGUI_REFRESH_MIN_TICKS 2ULL
#define SHELLGUI_EVENT_POLL_BACKOFF_TICKS 1ULL

#define SHELLGUI_LOG(fmt, ...)                                                                                              \
    do                                                                                                                      \
    {                                                                                                                       \
        unsigned long long __tick = (unsigned long long) sys_tick_get();                                                   \
        printf("[TheShellGUI t=%llu] " fmt, __tick, ##__VA_ARGS__);                                                       \
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
} shellgui_state_t;

static uint64_t shellgui_dbg_capture_calls = 0ULL;
static uint64_t shellgui_dbg_capture_bytes = 0ULL;
static uint64_t shellgui_dbg_capture_empty = 0ULL;
static uint64_t shellgui_dbg_capture_err = 0ULL;
static uint64_t shellgui_dbg_capture_max_ticks = 0ULL;
static uint64_t shellgui_dbg_refresh_max_ticks = 0ULL;
static uint64_t shellgui_dbg_refresh_slow = 0ULL;
static uint64_t shellgui_dbg_capture_slow = 0ULL;
static volatile uint64_t shellgui_dbg_mainloop_tick = 0ULL;
static volatile uint64_t shellgui_dbg_watchdog_log_tick = 0ULL;
static volatile bool shellgui_dbg_watchdog_run = false;
static uint64_t shellgui_dbg_capture_neg_rc = 0ULL;
static uint64_t shellgui_dbg_snapshot_epoch = 0ULL;

static void shellgui_debug_emit(const char* run_id,
                                const char* hypothesis_id,
                                const char* location,
                                const char* message,
                                unsigned long long v1,
                                unsigned long long v2,
                                unsigned long long v3)
{
    FILE* file = fopen("/home/alternant/TheOS-Reborn/.cursor/debug-f2d0c7.log", "a");
    if (!file)
        return;

    unsigned long long timestamp = (unsigned long long) sys_tick_get();
    // #region agent log
    fprintf(file,
            "{\"sessionId\":\"f2d0c7\",\"runId\":\"%s\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":{\"v1\":%llu,\"v2\":%llu,\"v3\":%llu},\"timestamp\":%llu}\n",
            run_id,
            hypothesis_id,
            location,
            message,
            v1,
            v2,
            v3,
            timestamp);
    // #endregion
    fclose(file);
}

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
    for (uint32_t i = 0U; i < state->line_count; i++)
    {
        uint32_t idx = (state->line_head + i) % SHELLGUI_LINES_MAX;
        shellgui_buf_append(out_text, out_size, state->lines[idx]);
        shellgui_buf_append(out_text, out_size, "\n");
    }

    if (state->stream_line_len > 0U)
        shellgui_buf_append(out_text, out_size, state->stream_line);
}

static bool shellgui_refresh_window(shellgui_state_t* state)
{
    if (!state || state->window_id == 0U)
        return false;

    char text[WS_WINDOW_BODY_TEXT_MAX + 1U];
    shellgui_compose_text(state, text, sizeof(text));
    if (strcmp(text, state->last_sent_text) == 0)
        return true;

    // #region agent log
    shellgui_debug_emit("run-4",
                        "HYP2",
                        "TheShellGUI/main.c:shellgui_refresh_window",
                        "refresh_pre_exchange",
                        (unsigned long long) state->window_id,
                        (unsigned long long) strlen(text),
                        (unsigned long long) state->client.sock_fd);
    // #endregion
    uint64_t refresh_start_tick = sys_tick_get();
    if (ws_client_set_window_text(&state->client, state->window_id, text) != 0)
    {
        // #region agent log
        shellgui_debug_emit("run-4",
                            "HYP2",
                            "TheShellGUI/main.c:shellgui_refresh_window",
                            "refresh_exchange_fail",
                            (unsigned long long) errno,
                            (unsigned long long) state->client.poll_event_inflight,
                            (unsigned long long) state->client.poll_event_cached);
        // #endregion
        return false;
    }
    uint64_t refresh_ticks = sys_tick_get() - refresh_start_tick;
    if (refresh_ticks > shellgui_dbg_refresh_max_ticks)
        shellgui_dbg_refresh_max_ticks = refresh_ticks;
    if (refresh_ticks > 20ULL)
    {
        shellgui_dbg_refresh_slow++;
        // #region agent log
        SHELLGUI_LOG("[AGENTDBG H27 REFRESH_STALL] ticks=%llu text_len=%u\n",
                     (unsigned long long) refresh_ticks,
                     (unsigned int) strlen(text));
        // #endregion
    }

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

    if (sys_console_route_set_sid((uint32_t) pid, SYS_CONSOLE_ROUTE_FLAG_CAPTURE) < 0)
    {
        SHELLGUI_LOG("console route set failed pid=%d errno=%d\n", (int) pid, errno);
        (void) kill(pid, SIGTERM);
        return false;
    }

    SHELLGUI_LOG("TheShell launched pid=%d\n", (int) pid);
    state->shell_pid = pid;
    state->shell_running = true;
    return true;
}

static bool shellgui_poll_capture(shellgui_state_t* state)
{
    if (!state || !state->shell_running || state->shell_pid <= 0)
        return false;

    bool changed = false;
    char capture[512];
    shellgui_dbg_capture_calls++;
    uint64_t capture_start_tick = sys_tick_get();
    for (;;)
    {
        int rc = sys_console_route_read_sid((uint32_t) state->shell_pid, capture, sizeof(capture));
        if (rc <= 0)
        {
            if (rc == 0)
                shellgui_dbg_capture_empty++;
            else
            {
                shellgui_dbg_capture_err++;
                shellgui_dbg_capture_neg_rc++;
            }
            break;
        }

        shellgui_dbg_capture_bytes += (uint64_t) rc;
        for (int i = 0; i < rc; i++)
            shellgui_stream_push_char(state, capture[i]);
        changed = true;
    }
    uint64_t capture_ticks = sys_tick_get() - capture_start_tick;
    if (capture_ticks > shellgui_dbg_capture_max_ticks)
        shellgui_dbg_capture_max_ticks = capture_ticks;
    if (capture_ticks > 20ULL)
    {
        shellgui_dbg_capture_slow++;
        // #region agent log
        SHELLGUI_LOG("[AGENTDBG H28 CAPTURE_STALL] ticks=%llu changed=%u\n",
                     (unsigned long long) capture_ticks,
                     changed ? 1U : 0U);
        // #endregion
    }
    if ((changed && (shellgui_dbg_capture_calls % 64ULL == 0ULL)) || shellgui_dbg_capture_neg_rc != 0ULL)
    {
        // #region agent log
        shellgui_debug_emit("run-4",
                            "HYP1",
                            "TheShellGUI/main.c:shellgui_poll_capture",
                            "capture_state",
                            (unsigned long long) state->stream_line_len,
                            (unsigned long long) state->line_count,
                            (unsigned long long) shellgui_dbg_capture_neg_rc);
        // #endregion
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

static void* shellgui_watchdog_main(void* opaque)
{
    (void) opaque;
    while (shellgui_dbg_watchdog_run)
    {
        uint64_t now_tick = sys_tick_get();
        uint64_t last_tick = shellgui_dbg_mainloop_tick;
        uint64_t gap = (now_tick > last_tick) ? (now_tick - last_tick) : 0ULL;
        uint64_t last_log = shellgui_dbg_watchdog_log_tick;
        if (gap > 40ULL && (now_tick > last_log) && (now_tick - last_log > 40ULL))
        {
            shellgui_dbg_watchdog_log_tick = now_tick;
            SHELLGUI_LOG("[AGENTDBG H25 LOOP_STALL] now=%llu last=%llu gap=%llu\n",
                         (unsigned long long) now_tick,
                         (unsigned long long) last_tick,
                         (unsigned long long) gap);
        }
        (void) usleep(4000U);
    }
    return NULL;
}

int main(void)
{
    shellgui_state_t state;
    memset(&state, 0, sizeof(state));
    state.shell_pid = -1;

    if (ws_client_connect(&state.client, WS_CLIENT_ROLE_SHELL_GUI) < 0)
    {
        SHELLGUI_LOG("ws_client_connect failed errno=%d\n", errno);
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
        SHELLGUI_LOG("create_window failed errno=%d\n", errno);
        ws_client_disconnect(&state.client);
        return 1;
    }
    SHELLGUI_LOG("window created id=%u\n", (unsigned int) state.window_id);

    if (!shellgui_launch_shell(&state))
        shellgui_push_line(&state, "[failed to start TheShell]");
    else
        shellgui_push_line(&state, "[TheShell started - keyboard is shared with other tty clients]");
    state.last_sent_text[0] = '\0';
    (void) shellgui_refresh_window(&state);
    state.text_dirty = false;
    state.next_refresh_tick = sys_tick_get() + SHELLGUI_REFRESH_MIN_TICKS;
    state.next_event_poll_tick = 0ULL;
    uint64_t debug_loop_max_ticks = 0ULL;
    uint64_t debug_poll_backoff_hits = 0ULL;
    uint64_t debug_refresh_ok = 0ULL;
    uint64_t debug_log_epoch = 0ULL;
    uint64_t debug_poll_call_max_ticks = 0ULL;
    uint64_t debug_poll_call_slow = 0ULL;
    uint64_t debug_capture_changes = 0ULL;
    uint64_t debug_key_events = 0ULL;
    uint64_t debug_close_events = 0ULL;
    uint64_t debug_key_gap_max_ticks = 0ULL;
    uint64_t debug_key_gap_over_20_ticks = 0ULL;
    uint64_t debug_last_key_tick = 0ULL;
    uint64_t debug_gui_last_key = 0ULL;
    uint64_t debug_gui_key_enter = 0ULL;
    uint64_t debug_gui_key_unhandled = 0ULL;
    uint64_t debug_gui_key_inject_ok = 0ULL;
    uint64_t debug_gui_key_inject_fail = 0ULL;
    uint64_t debug_gui_key_inject_skip = 0ULL;
    pthread_t watchdog_thread;
    bool watchdog_started = false;
    shellgui_dbg_watchdog_run = true;
    shellgui_dbg_mainloop_tick = sys_tick_get();
    if (pthread_create(&watchdog_thread, NULL, shellgui_watchdog_main, NULL) == 0)
        watchdog_started = true;

    bool should_exit = false;
    while (!should_exit)
    {
        uint64_t loop_start_tick = sys_tick_get();
        shellgui_dbg_mainloop_tick = loop_start_tick;
        bool changed = false;

        if (shellgui_poll_capture(&state))
        {
            changed = true;
            debug_capture_changes++;
        }
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
                uint64_t poll_call_start = sys_tick_get();
                if (ws_client_poll_event(&state.client, &event) < 0)
                {
                    int poll_errno = errno;
                    shellgui_dbg_mainloop_tick = sys_tick_get();
                    uint64_t poll_call_ticks = sys_tick_get() - poll_call_start;
                    if (poll_call_ticks > debug_poll_call_max_ticks)
                        debug_poll_call_max_ticks = poll_call_ticks;
                    if (poll_call_ticks > 2ULL)
                        debug_poll_call_slow++;
                    if (poll_call_ticks > 20ULL)
                    {
                        SHELLGUI_LOG("[AGENTDBG H26 POLL_STALL] rc=-1 ticks=%llu errno=%d\n",
                                     (unsigned long long) poll_call_ticks,
                                     errno);
                    }
                    // #region agent log
                    shellgui_debug_emit("run-4",
                                        "HYP3",
                                        "TheShellGUI/main.c:main_loop_poll",
                                        "poll_event_fail",
                                        (unsigned long long) poll_errno,
                                        (unsigned long long) state.client.poll_event_inflight,
                                        (unsigned long long) state.client.poll_event_cached);
                    // #endregion
                    errno = poll_errno;
                    if (poll_errno == EAGAIN)
                    {
                        debug_poll_backoff_hits++;
                        state.next_event_poll_tick = now_tick + SHELLGUI_EVENT_POLL_BACKOFF_TICKS;
                        break;
                    }

                    SHELLGUI_LOG("[AGENTDBG H33 POLL_FATAL] errno=%d inflight=%u cached=%u connected=%u win=%u\n",
                                 poll_errno,
                                 state.client.poll_event_inflight ? 1U : 0U,
                                 state.client.poll_event_cached ? 1U : 0U,
                                 state.client.connected ? 1U : 0U,
                                 (unsigned int) state.window_id);
                    should_exit = true;
                    break;
                }
                shellgui_dbg_mainloop_tick = sys_tick_get();
                uint64_t poll_call_ticks = sys_tick_get() - poll_call_start;
                if (poll_call_ticks > debug_poll_call_max_ticks)
                    debug_poll_call_max_ticks = poll_call_ticks;
                if (poll_call_ticks > 2ULL)
                    debug_poll_call_slow++;
                if (poll_call_ticks > 20ULL)
                {
                    SHELLGUI_LOG("[AGENTDBG H26 POLL_STALL] rc=0 ticks=%llu event=%u win=%u\n",
                                 (unsigned long long) poll_call_ticks,
                                 (unsigned int) event.type,
                                 (unsigned int) event.window_id);
                }

                if (event.type == WS_EVENT_CLOSE && event.window_id == state.window_id)
                {
                    debug_close_events++;
                    SHELLGUI_LOG("[AGENTDBG H33 CLOSE_EVENT] win=%u shell_pid=%d running=%u\n",
                                 (unsigned int) event.window_id,
                                 (int) state.shell_pid,
                                 state.shell_running ? 1U : 0U);
                    should_exit = true;
                    break;
                }
                if (event.type == WS_EVENT_KEY && event.window_id == state.window_id)
                {
                    debug_key_events++;
                    debug_gui_last_key = (uint64_t) event.key;
                    if (event.key == '\r' || event.key == '\n')
                        debug_gui_key_enter++;
                    uint8_t raw = (uint8_t) (event.key & 0xFFU);
                    /* Inject only make scancodes; break/extended noise desynchronizes stdio decoder state. */
                    if (raw == 0xE0U || (raw & 0x80U) != 0U)
                    {
                        debug_gui_key_inject_skip++;
                    }
                    else
                    {
                        if (state.shell_pid > 0 &&
                            sys_kbd_inject_scancode((uint32_t) state.shell_pid, raw) == 0)
                        {
                            debug_gui_key_inject_ok++;
                        }
                        else
                        {
                            debug_gui_key_inject_fail++;
                            debug_gui_key_unhandled++;
                        }
                    }
                    uint64_t key_tick = sys_tick_get();
                    if (debug_last_key_tick != 0ULL && key_tick > debug_last_key_tick)
                    {
                        uint64_t key_gap = key_tick - debug_last_key_tick;
                        if (key_gap > debug_key_gap_max_ticks)
                            debug_key_gap_max_ticks = key_gap;
                        if (key_gap > 20ULL)
                            debug_key_gap_over_20_ticks++;
                    }
                    debug_last_key_tick = key_tick;
                }
            }
        }
        uint64_t snapshot_epoch = now_tick / 250ULL;
        if (snapshot_epoch != 0ULL && snapshot_epoch > shellgui_dbg_snapshot_epoch)
        {
            shellgui_dbg_snapshot_epoch = snapshot_epoch;
            // #region agent log
            shellgui_debug_emit("run-4",
                                "HYP4",
                                "TheShellGUI/main.c:main_loop",
                                "state_snapshot",
                                (unsigned long long) state.stream_line_len,
                                (unsigned long long) state.line_count,
                                (unsigned long long) state.client.sock_fd);
            // #endregion
        }

        if (changed)
            state.text_dirty = true;

        now_tick = sys_tick_get();
        if (state.text_dirty &&
            (should_exit || now_tick >= state.next_refresh_tick))
        {
            if (shellgui_refresh_window(&state))
            {
                debug_refresh_ok++;
                state.text_dirty = false;
                state.next_refresh_tick = now_tick + SHELLGUI_REFRESH_MIN_TICKS;
            }
            else
            {
                SHELLGUI_LOG("[AGENTDBG H33 REFRESH_FAIL] errno=%d inflight=%u cached=%u dirty=%u\n",
                             errno,
                             state.client.poll_event_inflight ? 1U : 0U,
                             state.client.poll_event_cached ? 1U : 0U,
                             state.text_dirty ? 1U : 0U);
            }
        }

        if (state.text_dirty)
            (void) usleep(SHELLGUI_FRAME_TIME_US);
        else
            (void) usleep(SHELLGUI_IDLE_SLEEP_US);

        uint64_t loop_ticks = sys_tick_get() - loop_start_tick;
        if (loop_ticks > debug_loop_max_ticks)
            debug_loop_max_ticks = loop_ticks;
        uint64_t epoch = sys_tick_get() / 500ULL;
        if (epoch != 0ULL && epoch > debug_log_epoch)
        {
            debug_log_epoch = epoch;
            // #region agent log
            shellgui_debug_emit("run-2",
                                "H16",
                                "TheShellGUI/main.c:main_loop",
                                "loop_latency",
                                (unsigned long long) debug_loop_max_ticks,
                                (unsigned long long) debug_poll_backoff_hits,
                                (unsigned long long) debug_refresh_ok);
            SHELLGUI_LOG("[AGENTDBG H16 GUI_LOOP] loop_max=%llu poll_backoff=%llu refresh_ok=%llu dirty=%u\n",
                         (unsigned long long) debug_loop_max_ticks,
                         (unsigned long long) debug_poll_backoff_hits,
                         (unsigned long long) debug_refresh_ok,
                         state.text_dirty ? 1U : 0U);
            SHELLGUI_LOG("[AGENTDBG H18 GUI_POLL] poll_max=%llu poll_slow=%llu cap_changes=%llu\n",
                         (unsigned long long) debug_poll_call_max_ticks,
                         (unsigned long long) debug_poll_call_slow,
                         (unsigned long long) debug_capture_changes);
            SHELLGUI_LOG("[AGENTDBG H19 CAPTURE] calls=%llu bytes=%llu empty=%llu err=%llu\n",
                         (unsigned long long) shellgui_dbg_capture_calls,
                         (unsigned long long) shellgui_dbg_capture_bytes,
                         (unsigned long long) shellgui_dbg_capture_empty,
                         (unsigned long long) shellgui_dbg_capture_err);
            SHELLGUI_LOG("[AGENTDBG H27H28 STALL_MAX] refresh_max=%llu refresh_slow=%llu capture_max=%llu capture_slow=%llu\n",
                         (unsigned long long) shellgui_dbg_refresh_max_ticks,
                         (unsigned long long) shellgui_dbg_refresh_slow,
                         (unsigned long long) shellgui_dbg_capture_max_ticks,
                         (unsigned long long) shellgui_dbg_capture_slow);
            SHELLGUI_LOG("[AGENTDBG H20 KEYPATH] key_events=%llu close_events=%llu\n",
                         (unsigned long long) debug_key_events,
                         (unsigned long long) debug_close_events);
            SHELLGUI_LOG("[AGENTDBG H39 GUI_KEY_VALUES] last_key=%llu enter=%llu\n",
                         (unsigned long long) debug_gui_last_key,
                         (unsigned long long) debug_gui_key_enter);
            SHELLGUI_LOG("[AGENTDBG H43 GUI_KEY_UNHANDLED] total=%llu\n",
                         (unsigned long long) debug_gui_key_unhandled);
            SHELLGUI_LOG("[AGENTDBG H44 GUI_KEY_INJECT] ok=%llu fail=%llu\n",
                         (unsigned long long) debug_gui_key_inject_ok,
                         (unsigned long long) debug_gui_key_inject_fail);
            SHELLGUI_LOG("[AGENTDBG H45 GUI_KEY_INJECT_SKIP] skip=%llu\n",
                         (unsigned long long) debug_gui_key_inject_skip);
            shellgui_debug_emit("run-3",
                                "H23",
                                "TheShellGUI/main.c:main_loop",
                                "key_event_gap",
                                (unsigned long long) debug_key_gap_max_ticks,
                                (unsigned long long) debug_key_gap_over_20_ticks,
                                (unsigned long long) debug_key_events);
            SHELLGUI_LOG("[AGENTDBG H23 KEY_GAP] max_ticks=%llu over20=%llu key_events=%llu\n",
                         (unsigned long long) debug_key_gap_max_ticks,
                         (unsigned long long) debug_key_gap_over_20_ticks,
                         (unsigned long long) debug_key_events);
            // #endregion
        }
    }

    shellgui_dbg_watchdog_run = false;
    if (watchdog_started)
        (void) pthread_join(watchdog_thread, NULL);
    shellgui_cleanup(&state);
    SHELLGUI_LOG("exit\n");
    return 0;
}
