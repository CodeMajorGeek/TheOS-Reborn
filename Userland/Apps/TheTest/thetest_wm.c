#include "Includes/thetest_app.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>
#include <window.h>

#define THETEST_WM_LINES_MAX 80U
#define THETEST_WM_LINE_LEN  180U
#define THETEST_WM_STRESS_WORKERS 8U
#define THETEST_WM_STRESS_DURATION_MS 5000U
#define THETEST_WM_STRESS_SAMPLE_MS 200U

typedef struct thetest_wm_state
{
    ws_client_t client;
    uint32_t window_id;
    pid_t child_pid;
    bool child_running;
    char lines[THETEST_WM_LINES_MAX][THETEST_WM_LINE_LEN];
    uint32_t line_count;
    uint32_t line_head;
    char stream_line[THETEST_WM_LINE_LEN];
    size_t stream_line_len;
    bool text_dirty;
    char last_text[WS_WINDOW_BODY_TEXT_MAX + 1U];
} thetest_wm_state_t;

static void thetest_wm_copy_cstr(char* out, size_t out_size, const char* in)
{
    if (!out || out_size == 0U)
        return;
    out[0] = '\0';
    if (!in)
        return;
    size_t len = strlen(in);
    if (len >= out_size)
        len = out_size - 1U;
    memcpy(out, in, len);
    out[len] = '\0';
}

static void thetest_wm_buf_append(char* dst, size_t dst_size, const char* text)
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

static void thetest_wm_push_line(thetest_wm_state_t* state, const char* line)
{
    if (!state)
        return;
    uint32_t idx = 0U;
    if (state->line_count < THETEST_WM_LINES_MAX)
    {
        idx = (state->line_head + state->line_count) % THETEST_WM_LINES_MAX;
        state->line_count++;
    }
    else
    {
        idx = state->line_head;
        state->line_head = (state->line_head + 1U) % THETEST_WM_LINES_MAX;
    }
    thetest_wm_copy_cstr(state->lines[idx], sizeof(state->lines[idx]), line ? line : "");
}

static void thetest_wm_stream_flush(thetest_wm_state_t* state)
{
    if (!state || state->stream_line_len == 0U)
        return;
    state->stream_line[state->stream_line_len] = '\0';
    thetest_wm_push_line(state, state->stream_line);
    state->stream_line_len = 0U;
    state->stream_line[0] = '\0';
}

static void thetest_wm_stream_push(thetest_wm_state_t* state, char c)
{
    if (!state)
        return;
    if (c == '\n')
    {
        thetest_wm_stream_flush(state);
        return;
    }
    if (c == '\r')
    {
        state->stream_line_len = 0U;
        state->stream_line[0] = '\0';
        return;
    }
    if (c == '\b' || c == 127)
    {
        if (state->stream_line_len > 0U)
            state->stream_line[--state->stream_line_len] = '\0';
        return;
    }
    if ((unsigned char) c < 32U && c != '\t')
        return;
    if (state->stream_line_len + 1U >= sizeof(state->stream_line))
        return;
    state->stream_line[state->stream_line_len++] = c;
    state->stream_line[state->stream_line_len] = '\0';
}

static bool thetest_wm_refresh(thetest_wm_state_t* state)
{
    if (!state || state->window_id == 0U)
        return false;
    char text[WS_WINDOW_BODY_TEXT_MAX + 1U];
    text[0] = '\0';
    for (uint32_t i = 0U; i < state->line_count; i++)
    {
        uint32_t idx = (state->line_head + i) % THETEST_WM_LINES_MAX;
        thetest_wm_buf_append(text, sizeof(text), state->lines[idx]);
        thetest_wm_buf_append(text, sizeof(text), "\n");
    }
    if (state->stream_line_len > 0U)
        thetest_wm_buf_append(text, sizeof(text), state->stream_line);

    if (strcmp(text, state->last_text) == 0)
        return true;
    if (ws_client_set_window_text(&state->client, state->window_id, text) != 0)
        return false;

    thetest_wm_copy_cstr(state->last_text, sizeof(state->last_text), text);
    return true;
}

static void thetest_wm_cleanup(thetest_wm_state_t* state)
{
    if (!state)
        return;
    if (state->child_running && state->child_pid > 0)
        (void) kill(state->child_pid, SIGTERM);
    if (state->child_pid > 0)
    {
        (void) waitpid(state->child_pid, NULL, WNOHANG);
        (void) sys_console_route_set_sid((uint32_t) state->child_pid, 0U);
    }
    if (state->window_id != 0U)
        (void) ws_client_destroy_window(&state->client, state->window_id);
    ws_client_disconnect(&state->client);
}

static void thetest_wm_run_system_checks(thetest_wm_state_t* state)
{
    if (!state)
        return;

    syscall_cpu_info_t cpu_info;
    memset(&cpu_info, 0, sizeof(cpu_info));
    syscall_sched_info_t sched_info;
    memset(&sched_info, 0, sizeof(sched_info));
    syscall_rcu_info_t rcu_info;
    memset(&rcu_info, 0, sizeof(rcu_info));
    syscall_ahci_irq_info_t ahci_info;
    memset(&ahci_info, 0, sizeof(ahci_info));
    syscall_proc_info_t procs[SYS_PROC_MAX_ENTRIES];
    memset(procs, 0, sizeof(procs));

    bool cpu_ok = sys_cpu_info_get(&cpu_info) == 0;
    bool sched_ok = sys_sched_info_get(&sched_info) == 0;
    bool rcu_ok = sys_rcu_info_get(&rcu_info) == 0;
    bool ahci_ok = sys_ahci_irq_info_get(&ahci_info) == 0;
    uint32_t proc_total = 0U;
    int proc_copied = sys_proc_info_get(procs, SYS_PROC_MAX_ENTRIES, &proc_total);
    bool proc_ok = proc_copied >= 0;

    char line[THETEST_WM_LINE_LEN];
    snprintf(line, sizeof(line), "[TheTest][wm] checks cpu=%s sched=%s rcu=%s ahci=%s proc=%s",
             cpu_ok ? "OK" : "FAIL",
             sched_ok ? "OK" : "FAIL",
             rcu_ok ? "OK" : "FAIL",
             ahci_ok ? "OK" : "FAIL",
             proc_ok ? "OK" : "FAIL");
    thetest_wm_push_line(state, line);

    if (cpu_ok)
    {
        snprintf(line, sizeof(line), "[TheTest][wm] cpu online=%u tick_hz=%u current_cpu=%u",
                 (unsigned int) cpu_info.online_cpus,
                 (unsigned int) cpu_info.tick_hz,
                 (unsigned int) cpu_info.cpu_index);
        thetest_wm_push_line(state, line);
    }
    if (sched_ok)
    {
        snprintf(line, sizeof(line), "[TheTest][wm] sched preempt=%u rq_local=%u rq_total=%u",
                 (unsigned int) sched_info.preempt_count,
                 (unsigned int) sched_info.local_rq_depth,
                 (unsigned int) sched_info.total_rq_depth);
        thetest_wm_push_line(state, line);
    }
    if (proc_ok)
    {
        uint32_t running_now = 0U;
        uint32_t idle_now = 0U;
        for (int i = 0; i < proc_copied; i++)
        {
            if ((procs[i].flags & SYS_PROC_FLAG_ON_CPU) != 0U)
                running_now++;
            else
                idle_now++;
        }
        snprintf(line, sizeof(line), "[TheTest][wm] proc visible=%d total=%u running_now=%u idle=%u",
                 proc_copied,
                 (unsigned int) proc_total,
                 (unsigned int) running_now,
                 (unsigned int) idle_now);
        thetest_wm_push_line(state, line);
    }

    state->text_dirty = true;
}

static bool thetest_wm_poll_events(thetest_wm_state_t* state, bool* out_close_requested)
{
    if (!state || !out_close_requested)
        return false;
    *out_close_requested = false;

    for (;;)
    {
        ws_event_t event;
        if (ws_client_poll_event(&state->client, &event) < 0)
        {
            if (errno == EAGAIN)
                return true;
            return false;
        }
        if (event.type == WS_EVENT_CLOSE && event.window_id == state->window_id)
        {
            *out_close_requested = true;
            return true;
        }
    }
}

static void thetest_wm_stress_worker(uint64_t deadline_tick)
{
    volatile uint64_t acc = 0x123456789ABCDEF0ULL;
    while (sys_tick_get() < deadline_tick)
    {
        for (uint32_t i = 0; i < 4096U; i++)
            acc = (acc << 7U) ^ (acc >> 3U) ^ (uint64_t) i ^ 0x9E3779B97F4A7C15ULL;
        (void) sched_yield();
    }
    _exit((acc != 0ULL) ? 0 : 2);
}

static void thetest_wm_run_scheduler_stress(thetest_wm_state_t* state, bool* out_close_requested)
{
    if (!state || !out_close_requested)
        return;
    *out_close_requested = false;

    syscall_cpu_info_t cpu_info;
    memset(&cpu_info, 0, sizeof(cpu_info));
    uint32_t online_cpus = 1U;
    if (sys_cpu_info_get(&cpu_info) == 0 && cpu_info.online_cpus > 0U)
        online_cpus = cpu_info.online_cpus;

    char line[THETEST_WM_LINE_LEN];
    snprintf(line, sizeof(line),
             "[TheTest][wm] scheduler stress start workers=%u duration=%ums online_cpus=%u",
             (unsigned int) THETEST_WM_STRESS_WORKERS,
             (unsigned int) THETEST_WM_STRESS_DURATION_MS,
             (unsigned int) online_cpus);
    thetest_wm_push_line(state, line);
    state->text_dirty = true;
    (void) thetest_wm_refresh(state);

    pid_t workers[THETEST_WM_STRESS_WORKERS];
    memset(workers, 0, sizeof(workers));
    uint64_t end_tick = sys_tick_get() + THETEST_WM_STRESS_DURATION_MS;
    uint32_t spawned = 0U;
    for (uint32_t i = 0U; i < THETEST_WM_STRESS_WORKERS; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            break;
        if (pid == 0)
            thetest_wm_stress_worker(end_tick);
        workers[i] = pid;
        spawned++;
    }

    if (spawned == 0U)
    {
        thetest_wm_push_line(state, "[TheTest][wm] scheduler stress FAILED: spawn workers");
        state->text_dirty = true;
        return;
    }

    uint8_t cpu_seen[256];
    memset(cpu_seen, 0, sizeof(cpu_seen));
    uint32_t samples = 0U;
    uint32_t max_running_now = 0U;
    uint32_t max_total_rq = 0U;

    while (sys_tick_get() < end_tick)
    {
        bool close_requested = false;
        if (!thetest_wm_poll_events(state, &close_requested))
        {
            *out_close_requested = true;
            return;
        }
        if (close_requested)
        {
            *out_close_requested = true;
            return;
        }

        syscall_proc_info_t entries[SYS_PROC_MAX_ENTRIES];
        uint32_t total = 0U;
        int copied = sys_proc_info_get(entries, SYS_PROC_MAX_ENTRIES, &total);
        (void) total;

        syscall_sched_info_t sched_info;
        memset(&sched_info, 0, sizeof(sched_info));
        if (sys_sched_info_get(&sched_info) == 0 && sched_info.total_rq_depth > max_total_rq)
            max_total_rq = sched_info.total_rq_depth;

        if (copied > 0)
        {
            uint32_t running_now = 0U;
            for (int i = 0; i < copied; i++)
            {
                const syscall_proc_info_t* p = &entries[i];
                for (uint32_t w = 0U; w < spawned; w++)
                {
                    if (p->pid != (uint32_t) workers[w])
                        continue;
                    if (p->current_cpu != SYS_PROC_CPU_NONE)
                    {
                        running_now++;
                        if (p->current_cpu < 256U)
                            cpu_seen[p->current_cpu] = 1U;
                    }
                    if (p->last_cpu < 256U)
                        cpu_seen[p->last_cpu] = 1U;
                    break;
                }
            }
            if (running_now > max_running_now)
                max_running_now = running_now;
            samples++;
        }

        (void) usleep(THETEST_WM_STRESS_SAMPLE_MS * 1000U);
    }

    uint32_t exited_ok = 0U;
    for (uint32_t i = 0U; i < spawned; i++)
    {
        int status = 0;
        int rc = waitpid(workers[i], &status, 0);
        if (rc == workers[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            exited_ok++;
    }

    uint32_t cpu_coverage = 0U;
    for (uint32_t i = 0; i < 256U; i++)
    {
        if (cpu_seen[i] != 0U)
            cpu_coverage++;
    }

    uint32_t required_coverage = (online_cpus > 1U) ? 2U : 1U;
    bool pass = (exited_ok == spawned) && (cpu_coverage >= required_coverage) && (samples > 0U);

    snprintf(line, sizeof(line),
             "[TheTest][wm] scheduler stress %s workers_ok=%u/%u cpu_coverage=%u max_running_now=%u max_total_rq=%u",
             pass ? "PASS" : "FAIL",
             (unsigned int) exited_ok,
             (unsigned int) spawned,
             (unsigned int) cpu_coverage,
             (unsigned int) max_running_now,
             (unsigned int) max_total_rq);
    thetest_wm_push_line(state, line);
    state->text_dirty = true;
}

int thetest_run_wm(const char* self_path)
{
    (void) self_path;
    thetest_wm_state_t state;
    memset(&state, 0, sizeof(state));
    state.child_pid = -1;

    if (ws_client_connect(&state.client, WS_CLIENT_ROLE_GENERIC) < 0)
    {
        printf("[TheTest] wm connect failed errno=%d\n", errno);
        return 1;
    }

    ws_window_desc_t desc = {
        .x = 120,
        .y = 84,
        .width = 980U,
        .height = 640U,
        .color = 0x001A2330U,
        .border_color = 0x00445F7EU,
        .titlebar_color = 0x00344D69U,
        .visible = true,
        .frame_controls = true,
        .title = "TheTest --wm",
    };
    if (ws_client_create_window(&state.client, &desc, &state.window_id) < 0)
    {
        printf("[TheTest] wm create_window failed errno=%d\n", errno);
        ws_client_disconnect(&state.client);
        return 1;
    }

    thetest_wm_push_line(&state, "[TheTest][wm] suite start");
    thetest_wm_push_line(&state, "[TheTest][wm] note: CLI full suite skipped in WM to avoid instability");
    thetest_wm_run_system_checks(&state);
    (void) thetest_wm_refresh(&state);

    bool exit_loop = false;
    bool stress_done = false;
    while (!exit_loop)
    {
        bool close_requested = false;
        if (!thetest_wm_poll_events(&state, &close_requested))
        {
            exit_loop = true;
            break;
        }
        if (close_requested)
            exit_loop = true;

        if (state.text_dirty)
        {
            if (thetest_wm_refresh(&state))
                state.text_dirty = false;
        }

        if (!exit_loop && !stress_done)
        {
            bool stress_close = false;
            thetest_wm_run_scheduler_stress(&state, &stress_close);
            stress_done = true;
            state.text_dirty = true;
            (void) thetest_wm_refresh(&state);
            thetest_wm_push_line(&state, "[TheTest][wm] suite complete - close window to exit");
            state.text_dirty = true;
            if (stress_close)
                exit_loop = true;
        }

        (void) sched_yield();
    }

    thetest_wm_cleanup(&state);
    return 0;
}
