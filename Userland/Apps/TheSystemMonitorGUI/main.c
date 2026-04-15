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

#define MONGUI_LINES_MAX     72U
#define MONGUI_LINE_LEN      180U
#define MONGUI_REFRESH_MIN_TICKS 0ULL
#define MONGUI_EVENT_POLL_BACKOFF_TICKS 0ULL
#define MONGUI_FRAME_HEADER_PREFIX "TheSystemMonitor  frame="

#define MONGUI_LOG(fmt, ...)                                                                                               \
    do                                                                                                                      \
    {                                                                                                                       \
        unsigned long long __tick = (unsigned long long) sys_tick_get();                                                   \
        printf("[TheSystemMonitorGUI t=%llu] " fmt, __tick, ##__VA_ARGS__);                                               \
    }                                                                                                                       \
    while (0)

typedef struct mongui_state
{
    ws_client_t client;
    uint32_t window_id;
    pid_t monitor_pid;
    bool monitor_running;
    char lines[MONGUI_LINES_MAX][MONGUI_LINE_LEN];
    uint32_t line_count;
    uint32_t line_head;
    char stream_line[MONGUI_LINE_LEN];
    size_t stream_line_len;
    bool stream_pending_cr;
    bool text_dirty;
    uint64_t next_refresh_tick;
    uint64_t next_event_poll_tick;
    char last_sent_text[WS_WINDOW_BODY_TEXT_MAX + 1U];
} mongui_state_t;

static void mongui_copy_string_limit(char* out, size_t out_size, const char* in)
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

static void mongui_buf_append(char* dst, size_t dst_size, const char* text)
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

static void mongui_push_line(mongui_state_t* state, const char* line)
{
    if (!state)
        return;

    if (line &&
        strncmp(line, MONGUI_FRAME_HEADER_PREFIX, sizeof(MONGUI_FRAME_HEADER_PREFIX) - 1U) == 0)
    {
        state->line_count = 0U;
        state->line_head = 0U;
    }

    uint32_t index = 0U;
    if (state->line_count < MONGUI_LINES_MAX)
    {
        index = (state->line_head + state->line_count) % MONGUI_LINES_MAX;
        state->line_count++;
    }
    else
    {
        index = state->line_head;
        state->line_head = (state->line_head + 1U) % MONGUI_LINES_MAX;
    }

    mongui_copy_string_limit(state->lines[index], sizeof(state->lines[index]), line ? line : "");
}

static void mongui_stream_flush(mongui_state_t* state)
{
    if (!state || state->stream_line_len == 0U)
        return;

    state->stream_line[state->stream_line_len] = '\0';
    mongui_push_line(state, state->stream_line);
    state->stream_line_len = 0U;
    state->stream_line[0] = '\0';
    state->stream_pending_cr = false;
}

static void mongui_stream_push_char(mongui_state_t* state, char c)
{
    if (!state)
        return;

    if (c == '\r')
    {
        state->stream_pending_cr = true;
        return;
    }
    if (c == '\n')
    {
        mongui_stream_flush(state);
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

static void mongui_compose_text(const mongui_state_t* state, char* out_text, size_t out_size)
{
    if (!state || !out_text || out_size == 0U)
        return;

    out_text[0] = '\0';
    for (uint32_t i = 0U; i < state->line_count; i++)
    {
        uint32_t idx = (state->line_head + i) % MONGUI_LINES_MAX;
        mongui_buf_append(out_text, out_size, state->lines[idx]);
        mongui_buf_append(out_text, out_size, "\n");
    }

    if (state->stream_line_len > 0U)
        mongui_buf_append(out_text, out_size, state->stream_line);
}

static bool mongui_refresh_window(mongui_state_t* state)
{
    if (!state || state->window_id == 0U)
        return false;

    char text[WS_WINDOW_BODY_TEXT_MAX + 1U];
    mongui_compose_text(state, text, sizeof(text));
    if (strcmp(text, state->last_sent_text) == 0)
        return true;

    if (ws_client_set_window_text(&state->client, state->window_id, text) != 0)
        return false;

    mongui_copy_string_limit(state->last_sent_text, sizeof(state->last_sent_text), text);
    return true;
}

static bool mongui_launch_monitor(mongui_state_t* state)
{
    if (!state)
        return false;

    pid_t pid = fork();
    if (pid < 0)
    {
        MONGUI_LOG("fork failed errno=%d\n", errno);
        return false;
    }

    if (pid == 0)
    {
        char* const argv[] = {
            "TheSystemMonitor",
            "--interval",
            "500",
            "--no-input",
            "--no-clear",
            NULL
        };
        (void) execv("/bin/TheSystemMonitor", argv);
        _exit(127);
    }

    if (sys_console_route_set_sid((uint32_t) pid, SYS_CONSOLE_ROUTE_FLAG_CAPTURE) < 0)
    {
        MONGUI_LOG("console route set failed pid=%d errno=%d\n", (int) pid, errno);
        (void) kill(pid, SIGTERM);
        return false;
    }

    MONGUI_LOG("monitor launched pid=%d\n", (int) pid);
    state->monitor_pid = pid;
    state->monitor_running = true;
    return true;
}

static bool mongui_poll_capture(mongui_state_t* state)
{
    if (!state || !state->monitor_running || state->monitor_pid <= 0)
        return false;

    bool changed = false;
    char capture[512];
    for (;;)
    {
        int rc = sys_console_route_read_sid((uint32_t) state->monitor_pid, capture, sizeof(capture));
        if (rc <= 0)
            break;

        for (int i = 0; i < rc; i++)
            mongui_stream_push_char(state, capture[i]);
        changed = true;
    }

    return changed;
}

static bool mongui_poll_child_status(mongui_state_t* state)
{
    if (!state || !state->monitor_running || state->monitor_pid <= 0)
        return false;

    int status = 0;
    int rc = waitpid(state->monitor_pid, &status, WNOHANG);
    if (rc != state->monitor_pid)
        return false;

    state->monitor_running = false;
    mongui_stream_flush(state);
    if (WIFSIGNALED(status))
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "[TheSystemMonitor terminated by signal %d]", WTERMSIG(status));
        mongui_push_line(state, msg);
    }
    else if (WIFEXITED(status))
    {
        int code = WEXITSTATUS(status);
        if (code == 127)
            mongui_push_line(state, "[failed to exec TheSystemMonitor]");
        else
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "[TheSystemMonitor exited code=%d]", code);
            mongui_push_line(state, msg);
        }
    }

    (void) sys_console_route_set_sid((uint32_t) state->monitor_pid, 0U);
    return true;
}

static void mongui_cleanup(mongui_state_t* state)
{
    if (!state)
        return;

    if (state->monitor_running && state->monitor_pid > 0)
        (void) kill(state->monitor_pid, SIGTERM);
    if (state->monitor_pid > 0)
    {
        (void) waitpid(state->monitor_pid, NULL, WNOHANG);
        (void) sys_console_route_set_sid((uint32_t) state->monitor_pid, 0U);
    }

    if (state->window_id != 0U)
        (void) ws_client_destroy_window(&state->client, state->window_id);
    ws_client_disconnect(&state->client);
}

int main(void)
{
    mongui_state_t state;
    memset(&state, 0, sizeof(state));
    state.monitor_pid = -1;

    if (ws_client_connect(&state.client, WS_CLIENT_ROLE_SYSTEM_MONITOR_GUI) < 0)
    {
        MONGUI_LOG("ws_client_connect failed errno=%d\n", errno);
        return 1;
    }

    ws_window_desc_t window_desc = {
        .x = 110,
        .y = 70,
        .width = 980U,
        .height = 640U,
        .color = 0x001D2835U,
        .border_color = 0x00405B77U,
        .titlebar_color = 0x00344D69U,
        .visible = true,
        .frame_controls = true,
        .title = "TheSystemMonitorGUI"
    };

    if (ws_client_create_window(&state.client, &window_desc, &state.window_id) < 0)
    {
        MONGUI_LOG("create_window failed errno=%d\n", errno);
        ws_client_disconnect(&state.client);
        return 1;
    }
    MONGUI_LOG("window created id=%u\n", (unsigned int) state.window_id);

    if (!mongui_launch_monitor(&state))
        mongui_push_line(&state, "[failed to launch TheSystemMonitor]");
    else
        mongui_push_line(&state, "[TheSystemMonitor started]");
    state.last_sent_text[0] = '\0';
    (void) mongui_refresh_window(&state);
    state.text_dirty = false;
    state.next_refresh_tick = sys_tick_get() + MONGUI_REFRESH_MIN_TICKS;
    state.next_event_poll_tick = 0ULL;

    bool should_exit = false;
    while (!should_exit)
    {
        bool changed = false;

        if (mongui_poll_capture(&state))
            changed = true;
        if (mongui_poll_child_status(&state))
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
                    if (errno == EAGAIN)
                    {
                        state.next_event_poll_tick = now_tick + MONGUI_EVENT_POLL_BACKOFF_TICKS;
                        break;
                    }

                    should_exit = true;
                    break;
                }

                if (event.type == WS_EVENT_CLOSE && event.window_id == state.window_id)
                {
                    MONGUI_LOG("close event received win=%u\n", (unsigned int) event.window_id);
                    should_exit = true;
                    break;
                }
            }
        }

        if (changed)
            state.text_dirty = true;

        now_tick = sys_tick_get();
        if (state.text_dirty &&
            (should_exit || now_tick >= state.next_refresh_tick))
        {
            if (mongui_refresh_window(&state))
            {
                state.text_dirty = false;
                state.next_refresh_tick = now_tick + MONGUI_REFRESH_MIN_TICKS;
            }
        }

        (void) sched_yield();
    }

    mongui_cleanup(&state);
    MONGUI_LOG("exit\n");
    return 0;
}
