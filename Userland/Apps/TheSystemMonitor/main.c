#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#define MONITOR_DEFAULT_INTERVAL_MS 1000U
#define MONITOR_POLL_SLICE_MS       100U

typedef struct monitor_options
{
    bool once;
    bool help;
    bool no_input;
    bool no_clear;
    uint32_t interval_ms;
    uint32_t iterations;
} monitor_options_t;

typedef struct monitor_snapshot
{
    bool cpu_ok;
    bool sched_ok;
    bool ahci_ok;
    bool rcu_ok;
    bool procs_ok;
    syscall_cpu_info_t cpu;
    syscall_sched_info_t sched;
    syscall_ahci_irq_info_t ahci;
    syscall_rcu_info_t rcu;
    syscall_proc_info_t procs[SYS_PROC_MAX_ENTRIES];
    uint32_t proc_total;
    uint32_t proc_copied;
} monitor_snapshot_t;

static void monitor_print_help(const char* prog)
{
    const char* name = (prog && prog[0] != '\0') ? prog : "TheSystemMonitor";
    printf("Usage: %s [options]\n", name);
    printf("  --once                Print one snapshot and exit\n");
    printf("  --interval <ms>       Refresh interval in milliseconds (default: %u)\n", MONITOR_DEFAULT_INTERVAL_MS);
    printf("  --iterations <n>      Number of refresh iterations in live mode (0 = infinite)\n");
    printf("  --no-input            Do not poll keyboard (ESC disabled)\n");
    printf("  --no-clear            Do not clear terminal between refresh frames\n");
    printf("  -h, --help            Show this help\n");
    printf("\n");
    printf("Live mode key: press ESC to quit.\n");
}

static bool monitor_parse_u32(const char* text, uint32_t* out_value)
{
    if (!text || !out_value || text[0] == '\0')
        return false;

    char* end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (!end || *end != '\0')
        return false;
    if (parsed > 0xFFFFFFFFULL)
        return false;

    *out_value = (uint32_t) parsed;
    return true;
}

static bool monitor_parse_args(int argc, char** argv, monitor_options_t* out_opts)
{
    if (!argv || !out_opts)
        return false;

    monitor_options_t opts;
    opts.once = false;
    opts.help = false;
    opts.no_input = false;
    opts.no_clear = false;
    opts.interval_ms = MONITOR_DEFAULT_INTERVAL_MS;
    opts.iterations = 0U;

    for (int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if (!arg)
            continue;

        if (strcmp(arg, "--once") == 0)
        {
            opts.once = true;
            continue;
        }

        if (strcmp(arg, "--interval") == 0)
        {
            if (i + 1 >= argc)
                return false;

            uint32_t parsed = 0;
            if (!monitor_parse_u32(argv[i + 1], &parsed) || parsed == 0U)
                return false;

            opts.interval_ms = parsed;
            i++;
            continue;
        }

        if (strcmp(arg, "--iterations") == 0)
        {
            if (i + 1 >= argc)
                return false;

            uint32_t parsed = 0;
            if (!monitor_parse_u32(argv[i + 1], &parsed))
                return false;

            opts.iterations = parsed;
            i++;
            continue;
        }

        if (strcmp(arg, "--no-input") == 0)
        {
            opts.no_input = true;
            continue;
        }

        if (strcmp(arg, "--no-clear") == 0)
        {
            opts.no_clear = true;
            continue;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            opts.help = true;
            continue;
        }

        return false;
    }

    if (opts.once)
        opts.iterations = 1U;

    *out_opts = opts;
    return true;
}

static void monitor_sort_processes(syscall_proc_info_t* entries, uint32_t count)
{
    if (!entries || count <= 1U)
        return;

    for (uint32_t i = 1; i < count; i++)
    {
        syscall_proc_info_t key = entries[i];
        uint32_t j = i;
        while (j > 0U && entries[j - 1U].pid > key.pid)
        {
            entries[j] = entries[j - 1U];
            j--;
        }
        entries[j] = key;
    }
}

static const char* monitor_proc_kind(uint32_t flags)
{
    return (flags & SYS_PROC_FLAG_THREAD) ? "thread" : "process";
}

static const char* monitor_proc_domain(uint32_t domain)
{
    switch (domain)
    {
        case SYS_PROC_DOMAIN_KERNEL:
            return "krn";
        case SYS_PROC_DOMAIN_DRIVERLAND:
            return "drv";
        case SYS_PROC_DOMAIN_USERLAND:
            return "usr";
        default:
            return "?";
    }
}

static const char* monitor_proc_state(const syscall_proc_info_t* proc)
{
    if (!proc)
        return "?";

    uint32_t flags = proc->flags;
    if ((flags & SYS_PROC_FLAG_EXITING) == 0U)
    {
        if ((flags & SYS_PROC_FLAG_ON_CPU) != 0U)
            return "running";
        return "idle";
    }

    if ((flags & SYS_PROC_FLAG_TERMINATED_BY_SIGNAL) != 0U)
        return "signal";

    return "exiting";
}

static bool monitor_poll_escape(void)
{
    for (;;)
    {
        int code = sys_kbd_get_scancode();
        if (code <= 0)
            return false;

        uint8_t scancode = (uint8_t) code;
        if (scancode == 0x01U)
            return true;
    }
}

static void monitor_clear_screen(void)
{
    (void) clear_screen();
}

static uint64_t monitor_cpu_prev_exec;
static uint64_t monitor_cpu_prev_idle;
static bool monitor_cpu_have_prev;

static void monitor_print_cpu_usage(const syscall_cpu_info_t* cpu)
{
    if (!cpu)
        return;

    uint64_t exec = cpu->sched_exec_total;
    uint64_t idle = cpu->sched_idle_hlt_total;

    if (!monitor_cpu_have_prev)
    {
        monitor_cpu_prev_exec = exec;
        monitor_cpu_prev_idle = idle;
        monitor_cpu_have_prev = true;
        printf("  cpu_usage    : ... (next refresh)\n");
        return;
    }

    uint64_t d_exec = exec - monitor_cpu_prev_exec;
    uint64_t d_idle = idle - monitor_cpu_prev_idle;
    monitor_cpu_prev_exec = exec;
    monitor_cpu_prev_idle = idle;

    uint64_t den = d_exec + d_idle;
    if (den == 0ULL)
    {
        printf("  cpu_usage    : n/a\n");
        return;
    }

    uint32_t pct = (uint32_t) ((100ULL * d_exec + den / 2ULL) / den);
    if (pct > 100U)
        pct = 100U;

    printf("  cpu_usage    : ~%u%% (work vs idle-hlt samples)\n", (unsigned int) pct);
}

static void monitor_format_uptime(char* out, size_t out_size, uint64_t ticks, uint32_t tick_hz)
{
    if (!out || out_size == 0U)
        return;

    if (tick_hz == 0U)
    {
        snprintf(out, out_size, "n/a");
        return;
    }

    uint64_t total_seconds = ticks / (uint64_t) tick_hz;
    uint64_t days = total_seconds / 86400ULL;
    total_seconds %= 86400ULL;
    uint64_t hours = total_seconds / 3600ULL;
    total_seconds %= 3600ULL;
    uint64_t minutes = total_seconds / 60ULL;
    uint64_t seconds = total_seconds % 60ULL;

    if (days > 0ULL)
    {
        snprintf(out,
                 out_size,
                 "%llu d %02llu:%02llu:%02llu",
                 (unsigned long long) days,
                 (unsigned long long) hours,
                 (unsigned long long) minutes,
                 (unsigned long long) seconds);
        return;
    }

    snprintf(out,
             out_size,
             "%02llu:%02llu:%02llu",
             (unsigned long long) hours,
             (unsigned long long) minutes,
             (unsigned long long) seconds);
}

static void monitor_collect_snapshot(monitor_snapshot_t* out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    out->cpu_ok = (sys_cpu_info_get(&out->cpu) == 0);
    out->sched_ok = (sys_sched_info_get(&out->sched) == 0);
    out->ahci_ok = (sys_ahci_irq_info_get(&out->ahci) == 0);
    out->rcu_ok = (sys_rcu_info_get(&out->rcu) == 0);

    uint32_t total = 0;
    int copied = sys_proc_info_get(out->procs, SYS_PROC_MAX_ENTRIES, &total);
    if (copied >= 0)
    {
        out->procs_ok = true;
        out->proc_total = total;
        out->proc_copied = (uint32_t) copied;
        monitor_sort_processes(out->procs, out->proc_copied);
    }
}

static void monitor_render(const monitor_snapshot_t* snap,
                           uint32_t frame_index,
                           uint32_t interval_ms,
                           bool live_mode)
{
    if (!snap)
        return;

    if (live_mode)
        monitor_clear_screen();

    printf("TheSystemMonitor  frame=%u  interval=%ums\n", frame_index + 1U, interval_ms);
    if (live_mode)
        printf("Press ESC to quit.\n");
    printf("\n");

    if (snap->cpu_ok)
    {
        char uptime[48];
        monitor_format_uptime(uptime, sizeof(uptime), snap->cpu.ticks, snap->cpu.tick_hz);
        printf("System\n");
        printf("  uptime       : %s\n", uptime);
        printf("  ticks        : %llu\n", (unsigned long long) snap->cpu.ticks);
        printf("  tick_hz      : %u\n", snap->cpu.tick_hz);
        printf("  cpu_index    : %u\n", snap->cpu.cpu_index);
        printf("  apic_id      : %u\n", snap->cpu.apic_id);
        printf("  online_cpus  : %u\n", snap->cpu.online_cpus);
        monitor_print_cpu_usage(&snap->cpu);
    }
    else
    {
        printf("System\n");
        printf("  unavailable (SYS_CPU_INFO_GET failed)\n");
    }

    printf("\nScheduler\n");
    if (snap->sched_ok)
    {
        printf("  current_cpu  : %u\n", snap->sched.current_cpu);
        printf("  preempt_cnt  : %u\n", snap->sched.preempt_count);
        printf("  local_rq     : %u\n", snap->sched.local_rq_depth);
        printf("  total_rq     : %u\n", snap->sched.total_rq_depth);
    }
    else
    {
        printf("  unavailable (SYS_SCHED_INFO_GET failed)\n");
    }

    printf("\nI/O & RCU\n");
    if (snap->ahci_ok)
    {
        printf("  ahci_irq_mode    : %u\n", snap->ahci.mode);
        printf("  ahci_irq_count   : %llu\n", (unsigned long long) snap->ahci.count);
    }
    else
    {
        printf("  ahci             : unavailable\n");
    }

    if (snap->rcu_ok)
    {
        printf("  rcu_gp_seq       : %llu\n", (unsigned long long) snap->rcu.gp_seq);
        printf("  rcu_gp_target    : %llu\n", (unsigned long long) snap->rcu.gp_target);
        printf("  rcu_callbacks    : %llu\n", (unsigned long long) snap->rcu.callbacks_pending);
        printf("  rcu_read_depth   : %u\n", snap->rcu.local_read_depth);
    }
    else
    {
        printf("  rcu              : unavailable\n");
    }

    printf("\nProcesses\n");
    if (!snap->procs_ok)
    {
        printf("  unavailable (SYS_PROC_INFO_GET failed)\n");
        return;
    }

    uint32_t process_count = 0;
    uint32_t thread_count = 0;
    for (uint32_t i = 0; i < snap->proc_copied; i++)
    {
        if ((snap->procs[i].flags & SYS_PROC_FLAG_THREAD) != 0U)
            thread_count++;
        else
            process_count++;
    }

    printf("  visible=%u total=%u processes=%u threads=%u",
           snap->proc_copied,
           snap->proc_total,
           process_count,
           thread_count);
    if (snap->proc_total > snap->proc_copied)
        printf(" (truncated)");
    putc('\n');

    printf("\n");
    printf("  %-5s %-5s %-5s %-4s %-7s %-8s %-5s %-4s %-6s\n",
           "pid",
           "ppid",
           "own",
           "dom",
           "type",
           "state",
           "cpu",
           "sig",
           "exit");

    for (uint32_t i = 0; i < snap->proc_copied; i++)
    {
        const syscall_proc_info_t* proc = &snap->procs[i];
        const char* kind = monitor_proc_kind(proc->flags);
        const char* domain = monitor_proc_domain(proc->domain);
        const char* state = monitor_proc_state(proc);

        char cpu_text[12];
        if (proc->current_cpu == SYS_PROC_CPU_NONE)
            snprintf(cpu_text, sizeof(cpu_text), "-");
        else
            snprintf(cpu_text, sizeof(cpu_text), "%u", proc->current_cpu);

        char sig_text[12];
        if ((proc->flags & SYS_PROC_FLAG_TERMINATED_BY_SIGNAL) != 0U)
            snprintf(sig_text, sizeof(sig_text), "%u", proc->term_signal);
        else
            snprintf(sig_text, sizeof(sig_text), "-");

        printf("  %-5u %-5u %-5u %-4s %-7s %-8s %-5s %-4s %-6lld\n",
               proc->pid,
               proc->ppid,
               proc->owner_pid,
               domain,
               kind,
               state,
               cpu_text,
               sig_text,
               (long long) proc->exit_status);
    }
}

int main(int argc, char** argv)
{
    monitor_options_t opts;
    if (!monitor_parse_args(argc, argv, &opts))
    {
        monitor_print_help((argc > 0) ? argv[0] : "TheSystemMonitor");
        return 1;
    }

    if (opts.help)
    {
        monitor_print_help((argc > 0) ? argv[0] : "TheSystemMonitor");
        return 0;
    }

    uint32_t frame = 0;
    for (;;)
    {
        monitor_snapshot_t snapshot;
        monitor_collect_snapshot(&snapshot);
        monitor_render(&snapshot, frame, opts.interval_ms, (!opts.once && !opts.no_clear));
        frame++;

        if (opts.once)
            break;
        if (opts.iterations > 0U && frame >= opts.iterations)
            break;

        uint32_t remaining = opts.interval_ms;
        while (remaining > 0U)
        {
            if (!opts.no_input && monitor_poll_escape())
                return 0;

            uint32_t slice = (remaining > MONITOR_POLL_SLICE_MS) ? MONITOR_POLL_SLICE_MS : remaining;
            (void) sys_sleep_ms(slice);
            remaining -= slice;
        }
    }

    return 0;
}
