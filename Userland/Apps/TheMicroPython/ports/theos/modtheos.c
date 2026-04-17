#include <stdio.h>
#include <string.h>
#include <syscall.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/runtime.h"
#include "py/qstr.h"

#define THEOS_AGENT_LOG_PATH "/home/codemajorgeek/TheOS-Reborn/.cursor/debug-291fd6.log"

// #region agent log
static void theos_agent_log_json(const char *hypothesis_id, const char *location, const char *message, const char *json_data)
{
    FILE *f = fopen(THEOS_AGENT_LOG_PATH, "a");
    if (!f)
        return;
    long long ts = (long long) sys_tick_get();
    fprintf(f,
            "{\"sessionId\":\"291fd6\",\"runId\":\"pre-fix\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
            hypothesis_id ? hypothesis_id : "",
            location ? location : "",
            message ? message : "",
            json_data ? json_data : "{}",
            ts);
    fclose(f);
}

static void theos_agent_ctor(void) __attribute__((constructor));
static void theos_agent_ctor(void)
{
    theos_agent_log_json("H1", "modtheos.c:ctor", "theos_user_cmodule linked (constructor ran)",
                         "{\"theos_user_cmodule\":true}");
}
// #endregion

static mp_obj_t theos_time_ticks_ms(void)
{
    return mp_obj_new_int_from_uint((mp_uint_t) sys_tick_get());
}

static mp_obj_t theos_time_sleep_ms(mp_obj_t ms_in)
{
    uint32_t ms = (uint32_t) mp_obj_get_int(ms_in);
    int r = sys_sleep_ms(ms);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_time_rtc_get(void)
{
    return mp_obj_new_int_from_ull(sys_rtc_time_get());
}

static mp_obj_t theos_cpu_info(void)
{
    syscall_cpu_info_t info;
    memset(&info, 0, sizeof(info));
    int r = sys_cpu_info_get(&info);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[7] = {
        mp_obj_new_int(info.cpu_index),
        mp_obj_new_int(info.apic_id),
        mp_obj_new_int(info.online_cpus),
        mp_obj_new_int(info.tick_hz),
        mp_obj_new_int_from_ull(info.ticks),
        mp_obj_new_int_from_ull(info.sched_exec_total),
        mp_obj_new_int_from_ull(info.sched_idle_hlt_total),
    };
    return mp_obj_new_tuple(7, items);
}

static mp_obj_t theos_sched_info(void)
{
    syscall_sched_info_t info;
    memset(&info, 0, sizeof(info));
    int r = sys_sched_info_get(&info);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[4] = {
        mp_obj_new_int(info.current_cpu),
        mp_obj_new_int(info.preempt_count),
        mp_obj_new_int(info.local_rq_depth),
        mp_obj_new_int(info.total_rq_depth),
    };
    return mp_obj_new_tuple(4, items);
}

static mp_obj_t theos_ahci_irq_info(void)
{
    syscall_ahci_irq_info_t info;
    memset(&info, 0, sizeof(info));
    int r = sys_ahci_irq_info_get(&info);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[3] = {
        mp_obj_new_int(info.mode),
        mp_obj_new_int(info.reserved),
        mp_obj_new_int_from_ull(info.count),
    };
    return mp_obj_new_tuple(3, items);
}

static mp_obj_t theos_rcu_sync(void)
{
    int r = sys_rcu_sync();
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_rcu_info(void)
{
    syscall_rcu_info_t info;
    memset(&info, 0, sizeof(info));
    int r = sys_rcu_info_get(&info);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[5] = {
        mp_obj_new_int_from_ull(info.gp_seq),
        mp_obj_new_int_from_ull(info.gp_target),
        mp_obj_new_int_from_ull(info.callbacks_pending),
        mp_obj_new_int(info.local_read_depth),
        mp_obj_new_int(info.local_preempt_count),
    };
    return mp_obj_new_tuple(5, items);
}

static mp_obj_t theos_proc_snapshot(void)
{
    syscall_proc_info_t entries[SYS_PROC_MAX_ENTRIES];
    uint32_t total = 0;
    memset(entries, 0, sizeof(entries));
    int r = sys_proc_info_get(entries, SYS_PROC_MAX_ENTRIES, &total);
    if (r < 0)
        mp_raise_OSError(-r);
    if (total > SYS_PROC_MAX_ENTRIES)
        mp_raise_OSError(MP_EINVAL);

    mp_obj_t rows[SYS_PROC_MAX_ENTRIES];
    for (uint32_t i = 0; i < total; i++)
    {
        const syscall_proc_info_t *p = &entries[i];
        mp_obj_t cols[9] = {
            mp_obj_new_int(p->pid),
            mp_obj_new_int(p->ppid),
            mp_obj_new_int(p->owner_pid),
            mp_obj_new_int(p->domain),
            mp_obj_new_int(p->flags),
            mp_obj_new_int(p->current_cpu),
            mp_obj_new_int(p->last_cpu),
            mp_obj_new_int(p->term_signal),
            mp_obj_new_int_from_ll(p->exit_status),
        };
        rows[i] = mp_obj_new_tuple(9, cols);
    }

    return mp_obj_new_list((size_t) total, rows);
}

static mp_obj_t theos_console_write(mp_obj_t data_in)
{
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int r = sys_console_write(bufinfo.buf, bufinfo.len);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_obj_new_int(r);
}

static mp_obj_t theos_kdebug_write(mp_obj_t data_in)
{
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int r = sys_kdebug_write(bufinfo.buf, bufinfo.len);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_obj_new_int(r);
}

static mp_obj_t theos_console_route_set(mp_obj_t flags_in)
{
    uint32_t flags = (uint32_t) mp_obj_get_int(flags_in);
    int r = sys_console_route_set(flags);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_console_route_read(void)
{
    const size_t cap = 4096U;
    byte *buf = m_new(byte, cap);
    int r = sys_console_route_read(buf, cap);
    if (r < 0)
    {
        m_del(byte, buf, cap);
        mp_raise_OSError(-r);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t) r);
    m_del(byte, buf, cap);
    return out;
}

static mp_obj_t theos_console_route_set_sid(mp_obj_t sid_in, mp_obj_t flags_in)
{
    uint32_t sid = (uint32_t) mp_obj_get_int(sid_in);
    uint32_t flags = (uint32_t) mp_obj_get_int(flags_in);
    int r = sys_console_route_set_sid(sid, flags);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_console_route_read_sid(mp_obj_t sid_in)
{
    uint32_t sid = (uint32_t) mp_obj_get_int(sid_in);
    const size_t cap = 4096U;
    byte *buf = m_new(byte, cap);
    int r = sys_console_route_read_sid(sid, buf, cap);
    if (r < 0)
    {
        m_del(byte, buf, cap);
        mp_raise_OSError(-r);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t) r);
    m_del(byte, buf, cap);
    return out;
}

static mp_obj_t theos_console_route_input_write_sid(mp_obj_t sid_in, mp_obj_t data_in)
{
    uint32_t sid = (uint32_t) mp_obj_get_int(sid_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int r = sys_console_route_input_write_sid(sid, bufinfo.buf, bufinfo.len);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_obj_new_int(r);
}

static mp_obj_t theos_console_route_input_read(void)
{
    const size_t cap = 4096U;
    byte *buf = m_new(byte, cap);
    int r = sys_console_route_input_read(buf, cap);
    if (r < 0)
    {
        m_del(byte, buf, cap);
        mp_raise_OSError(-r);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t) r);
    m_del(byte, buf, cap);
    return out;
}

static mp_obj_t theos_kbd_get_scancode(void)
{
    return mp_obj_new_int(sys_kbd_get_scancode());
}

static mp_obj_t theos_kbd_inject_scancode(mp_obj_t pid_in, mp_obj_t sc_in)
{
    uint32_t pid = (uint32_t) mp_obj_get_int(pid_in);
    uint8_t sc = (uint8_t) mp_obj_get_int(sc_in);
    int r = sys_kbd_inject_scancode(pid, sc);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_kbd_capture_set(mp_obj_t owner_in)
{
    uint32_t owner = (uint32_t) mp_obj_get_int(owner_in);
    int r = sys_kbd_capture_set(owner);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_mouse_get_event(void)
{
    syscall_mouse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    int r = sys_mouse_get_event(&ev);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[3] = {
        mp_obj_new_int(ev.dx),
        mp_obj_new_int(ev.dy),
        mp_obj_new_int(ev.buttons),
    };
    return mp_obj_new_tuple(3, items);
}

static mp_obj_t theos_mouse_debug_info(void)
{
    syscall_mouse_debug_info_t info;
    memset(&info, 0, sizeof(info));
    int r = sys_mouse_debug_info_get(&info);
    if (r < 0)
        mp_raise_OSError(-r);

    mp_obj_t items[22] = {
        mp_obj_new_int_from_ull(info.irq12_callbacks),
        mp_obj_new_int_from_ull(info.irq12_bytes_total),
        mp_obj_new_int_from_ull(info.irq12_aux_bytes),
        mp_obj_new_int_from_ull(info.irq12_non_aux_bytes),
        mp_obj_new_int_from_ull(info.irq12_drain_budget_hits),
        mp_obj_new_int_from_ull(info.irq1_aux_bytes),
        mp_obj_new_int_from_ull(info.poll_cycles),
        mp_obj_new_int_from_ull(info.poll_bytes_total),
        mp_obj_new_int_from_ull(info.poll_aux_bytes),
        mp_obj_new_int_from_ull(info.poll_non_aux_bytes),
        mp_obj_new_int_from_ull(info.events_pushed),
        mp_obj_new_int_from_ull(info.events_dropped_full),
        mp_obj_new_int_from_ull(info.events_popped),
        mp_obj_new_int_from_ull(info.get_event_empty),
        mp_obj_new_int_from_ull(info.packet_sync_drops),
        mp_obj_new_int_from_ull(info.packet_overflow_drops),
        mp_obj_new_int(info.queue_count),
        mp_obj_new_int(info.queue_write_pos),
        mp_obj_new_int(info.queue_read_pos),
        mp_obj_new_int(info.packet_index),
        mp_obj_new_int(info.ready),
        mp_obj_new_int_from_ull(info.forced_request_attempts),
        mp_obj_new_int_from_ull(info.forced_request_success),
        mp_obj_new_int_from_ull(info.forced_request_fail),
    };
    return mp_obj_new_tuple(22, items);
}

static mp_obj_t theos_file_read(mp_obj_t path_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    int fd = sys_open(path, SYS_OPEN_READ);
    if (fd < 0)
        mp_raise_OSError(MP_ENOENT);

    int64_t sz64 = sys_lseek(fd, 0, SYS_SEEK_END);
    if (sz64 < 0)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOENT);
    }
    if (sys_lseek(fd, 0, SYS_SEEK_SET) < 0)
    {
        (void) sys_close(fd);
        mp_raise_OSError(MP_ENOENT);
    }

    size_t sz = (size_t) sz64;
    byte *buf = m_new(byte, sz);
    size_t got = 0;
    while (got < sz)
    {
        int r = sys_read(fd, buf + got, sz - got);
        if (r < 0)
        {
            m_del(byte, buf, sz);
            (void) sys_close(fd);
            mp_raise_OSError(-r);
        }
        if (r == 0)
            break;
        got += (size_t) r;
    }
    (void) sys_close(fd);
    return mp_obj_new_bytes(buf, got);
}

static mp_obj_t theos_file_write(mp_obj_t path_in, mp_obj_t data_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    uint64_t flags = SYS_OPEN_WRITE | SYS_OPEN_CREATE | SYS_OPEN_TRUNC;
    int fd = sys_open(path, flags);
    if (fd < 0)
        mp_raise_OSError(MP_ENOENT);

    size_t wrote = 0;
    while (wrote < bufinfo.len)
    {
        int r = sys_write(fd, (const uint8_t *) bufinfo.buf + wrote, bufinfo.len - wrote);
        if (r < 0)
        {
            (void) sys_close(fd);
            mp_raise_OSError(-r);
        }
        if (r == 0)
            break;
        wrote += (size_t) r;
    }
    (void) sys_close(fd);
    return mp_obj_new_int_from_uint((mp_uint_t) wrote);
}

static mp_obj_t theos_fs_is_dir(mp_obj_t path_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    return mp_obj_new_int(fs_is_dir(path));
}

static mp_obj_t theos_fs_mkdir(mp_obj_t path_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    return mp_obj_new_int(fs_mkdir(path));
}

static mp_obj_t theos_fs_readdir(mp_obj_t path_in, mp_obj_t index_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    uint64_t index = (uint64_t) mp_obj_get_ll(index_in);
    syscall_dirent_t ent;
    memset(&ent, 0, sizeof(ent));
    int r = fs_readdir(path, index, &ent);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[3] = {
        mp_obj_new_int(ent.d_ino),
        mp_obj_new_int(ent.d_type),
        mp_obj_new_str_copy(&mp_type_str, (const byte *) ent.d_name, strlen(ent.d_name)),
    };
    return mp_obj_new_tuple(3, items);
}

static mp_obj_t theos_yield(void)
{
    int r = sys_yield();
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_exit(mp_obj_t status_in)
{
    int st = mp_obj_get_int(status_in);
    sys_exit(st);
    return mp_const_none;
}

static mp_obj_t theos_fork(void)
{
    return mp_obj_new_int(sys_fork());
}

static mp_obj_t theos_execve(mp_obj_t path_in, mp_obj_t argv_in, mp_obj_t envp_in)
{
    const char *path = mp_obj_str_get_str(path_in);
    size_t argc = 0;
    mp_obj_t *argv_objs = NULL;
    mp_obj_get_array(argv_in, &argc, &argv_objs);

    const char **argv = m_new(const char *, argc + 1U);
    for (size_t i = 0; i < argc; i++)
        argv[i] = mp_obj_str_get_str(argv_objs[i]);
    argv[argc] = NULL;

    const char **envp = NULL;
    size_t envc = 0;
    if (envp_in != mp_const_none)
    {
        mp_obj_t *env_objs = NULL;
        mp_obj_get_array(envp_in, &envc, &env_objs);
        envp = m_new(const char *, envc + 1U);
        for (size_t i = 0; i < envc; i++)
            envp[i] = mp_obj_str_get_str(env_objs[i]);
        envp[envc] = NULL;
    }

    int r = sys_execve(path, argv, envp);
    m_del(const char *, argv, argc + 1U);
    if (envp)
        m_del(const char *, envp, envc + 1U);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_waitpid(mp_obj_t pid_in, mp_obj_t flags_in)
{
    int pid = mp_obj_get_int(pid_in);
    (void) flags_in;
    int status = 0;
    int signal = 0;
    int r = sys_waitpid(pid, &status, &signal);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[3] = {
        mp_obj_new_int(r),
        mp_obj_new_int(status),
        mp_obj_new_int(signal),
    };
    return mp_obj_new_tuple(3, items);
}

static mp_obj_t theos_kill(mp_obj_t pid_in, mp_obj_t sig_in)
{
    int pid = mp_obj_get_int(pid_in);
    int sig = mp_obj_get_int(sig_in);
    int r = sys_kill(pid, sig);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_power(mp_obj_t cmd_in, mp_obj_t arg_in)
{
    uint32_t cmd = (uint32_t) mp_obj_get_int(cmd_in);
    uint32_t arg = (uint32_t) mp_obj_get_int(arg_in);
    int r = sys_power(cmd, arg);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_shutdown(void)
{
    int r = sys_shutdown();
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_sleep_state(mp_obj_t state_in)
{
    uint32_t st = (uint32_t) mp_obj_get_int(state_in);
    int r = sys_sleep(st);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_reboot(void)
{
    int r = sys_reboot();
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_map_ex(size_t n_args, const mp_obj_t *args)
{
    (void) n_args;
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(args[0]);
    size_t len = (size_t) mp_obj_get_ll(args[1]);
    uint64_t prot = (uint64_t) mp_obj_get_ll(args[2]);
    uint64_t flags = (uint64_t) mp_obj_get_ll(args[3]);
    int fd = mp_obj_get_int(args[4]);
    uint64_t off = (uint64_t) mp_obj_get_ll(args[5]);
    void *p = sys_map_ex(addr, len, prot, flags, fd, off);
    if (!p)
        mp_raise_OSError(MP_ENOMEM);
    return mp_obj_new_int_from_ull((unsigned long long)(uintptr_t) p);
}

static mp_obj_t theos_map(mp_obj_t addr_in, mp_obj_t len_in, mp_obj_t prot_in)
{
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(addr_in);
    size_t len = (size_t) mp_obj_get_ll(len_in);
    uint64_t prot = (uint64_t) mp_obj_get_ll(prot_in);
    void *p = sys_map(addr, len, prot);
    if (!p)
        mp_raise_OSError(MP_ENOMEM);
    return mp_obj_new_int_from_ull((unsigned long long)(uintptr_t) p);
}

static mp_obj_t theos_unmap(mp_obj_t addr_in, mp_obj_t len_in)
{
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(addr_in);
    size_t len = (size_t) mp_obj_get_ll(len_in);
    int r = sys_unmap(addr, len);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_mprotect(mp_obj_t addr_in, mp_obj_t len_in, mp_obj_t prot_in)
{
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(addr_in);
    size_t len = (size_t) mp_obj_get_ll(len_in);
    uint64_t prot = (uint64_t) mp_obj_get_ll(prot_in);
    int r = sys_mprotect(addr, len, prot);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_thread_create_ex(mp_obj_t rip_in, mp_obj_t arg_in, mp_obj_t sp_in, mp_obj_t fs_in)
{
    uintptr_t rip = (uintptr_t) mp_obj_get_ll(rip_in);
    uintptr_t arg = (uintptr_t) mp_obj_get_ll(arg_in);
    uintptr_t sp = (uintptr_t) mp_obj_get_ll(sp_in);
    uintptr_t fs = (uintptr_t) mp_obj_get_ll(fs_in);
    return mp_obj_new_int(sys_thread_create_ex(rip, arg, sp, fs));
}

static mp_obj_t theos_thread_create(mp_obj_t rip_in, mp_obj_t arg_in, mp_obj_t sp_in)
{
    uintptr_t rip = (uintptr_t) mp_obj_get_ll(rip_in);
    uintptr_t arg = (uintptr_t) mp_obj_get_ll(arg_in);
    uintptr_t sp = (uintptr_t) mp_obj_get_ll(sp_in);
    return mp_obj_new_int(sys_thread_create(rip, arg, sp));
}

static mp_obj_t theos_thread_join(mp_obj_t tid_in)
{
    int tid = mp_obj_get_int(tid_in);
    uint64_t retval = 0;
    int r = sys_thread_join(tid, &retval);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[2] = {
        mp_obj_new_int(r),
        mp_obj_new_int_from_ull(retval),
    };
    return mp_obj_new_tuple(2, items);
}

static mp_obj_t theos_thread_exit(mp_obj_t rv_in)
{
    uint64_t rv = (uint64_t) mp_obj_get_ll(rv_in);
    sys_thread_exit(rv);
    return mp_const_none;
}

static mp_obj_t theos_thread_self(void)
{
    return mp_obj_new_int(sys_thread_self());
}

static mp_obj_t theos_thread_set_fsbase(mp_obj_t fs_in)
{
    uintptr_t fs = (uintptr_t) mp_obj_get_ll(fs_in);
    int r = sys_thread_set_fsbase(fs);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_thread_get_fsbase(void)
{
    return mp_obj_new_int_from_ull((unsigned long long) sys_thread_get_fsbase());
}

static mp_obj_t theos_ipc_pipe(void)
{
    int fds[2] = { -1, -1 };
    int r = sys_pipe(fds);
    if (r < 0)
        mp_raise_OSError(-r);
    mp_obj_t items[2] = {
        mp_obj_new_int(fds[0]),
        mp_obj_new_int(fds[1]),
    };
    return mp_obj_new_tuple(2, items);
}

static mp_obj_t theos_ipc_futex(mp_obj_t uaddr_in, mp_obj_t op_in, mp_obj_t val_in, mp_obj_t timeout_in)
{
    void *uaddr = (void *)(uintptr_t) mp_obj_get_ll(uaddr_in);
    int op = mp_obj_get_int(op_in);
    int val = mp_obj_get_int(val_in);
    unsigned int timeout_ms = (unsigned int) mp_obj_get_int(timeout_in);
    return mp_obj_new_int(sys_futex(uaddr, op, val, timeout_ms));
}

static mp_obj_t theos_ipc_shmget(mp_obj_t key_in, mp_obj_t size_in, mp_obj_t flags_in)
{
    int key = mp_obj_get_int(key_in);
    size_t size = (size_t) mp_obj_get_ll(size_in);
    int flags = mp_obj_get_int(flags_in);
    return mp_obj_new_int(sys_shmget(key, size, flags));
}

static mp_obj_t theos_ipc_shmat(mp_obj_t id_in, mp_obj_t addr_in)
{
    int id = mp_obj_get_int(id_in);
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(addr_in);
    long p = sys_shmat(id, addr);
    if (p < 0)
        mp_raise_OSError((int) -p);
    return mp_obj_new_int_from_ull((unsigned long long)(uintptr_t) p);
}

static mp_obj_t theos_ipc_shmdt(mp_obj_t addr_in)
{
    void *addr = (void *)(uintptr_t) mp_obj_get_ll(addr_in);
    int r = sys_shmdt(addr);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_ipc_shmctl(mp_obj_t id_in, mp_obj_t cmd_in)
{
    int id = mp_obj_get_int(id_in);
    int cmd = mp_obj_get_int(cmd_in);
    return mp_obj_new_int(sys_shmctl(id, cmd));
}

static mp_obj_t theos_ipc_msgget(mp_obj_t key_in, mp_obj_t flags_in)
{
    int key = mp_obj_get_int(key_in);
    int flags = mp_obj_get_int(flags_in);
    return mp_obj_new_int(sys_msgget(key, flags));
}

static mp_obj_t theos_ipc_msgsnd(mp_obj_t id_in, mp_obj_t msg_in, mp_obj_t sz_in, mp_obj_t fl_in)
{
    int id = mp_obj_get_int(id_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(msg_in, &bufinfo, MP_BUFFER_READ);
    size_t sz = (size_t) mp_obj_get_ll(sz_in);
    int fl = mp_obj_get_int(fl_in);
    int r = sys_msgsnd(id, bufinfo.buf, sz, fl);
    if (r < 0)
        mp_raise_OSError(-r);
    return mp_const_none;
}

static mp_obj_t theos_ipc_msgrcv(mp_obj_t id_in, mp_obj_t msg_in, mp_obj_t sz_in, mp_obj_t typ_in, mp_obj_t fl_in)
{
    int id = mp_obj_get_int(id_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(msg_in, &bufinfo, MP_BUFFER_WRITE);
    size_t sz = (size_t) mp_obj_get_ll(sz_in);
    long typ = (long) mp_obj_get_ll(typ_in);
    int fl = mp_obj_get_int(fl_in);
    long r = sys_msgrcv(id, bufinfo.buf, sz, typ, fl);
    if (r < 0)
        mp_raise_OSError((int) -r);
    return mp_obj_new_int_from_ll(r);
}

static MP_DEFINE_CONST_FUN_OBJ_0(theos_time_ticks_ms_obj, theos_time_ticks_ms);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_time_sleep_ms_obj, theos_time_sleep_ms);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_time_rtc_get_obj, theos_time_rtc_get);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_cpu_info_obj, theos_cpu_info);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_sched_info_obj, theos_sched_info);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_ahci_irq_info_obj, theos_ahci_irq_info);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_rcu_sync_obj, theos_rcu_sync);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_rcu_info_obj, theos_rcu_info);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_proc_snapshot_obj, theos_proc_snapshot);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_console_write_obj, theos_console_write);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_kdebug_write_obj, theos_kdebug_write);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_console_route_set_obj, theos_console_route_set);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_console_route_read_obj, theos_console_route_read);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_console_route_set_sid_obj, theos_console_route_set_sid);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_console_route_read_sid_obj, theos_console_route_read_sid);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_console_route_input_write_sid_obj, theos_console_route_input_write_sid);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_console_route_input_read_obj, theos_console_route_input_read);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_kbd_get_scancode_obj, theos_kbd_get_scancode);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_kbd_inject_scancode_obj, theos_kbd_inject_scancode);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_kbd_capture_set_obj, theos_kbd_capture_set);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_mouse_get_event_obj, theos_mouse_get_event);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_mouse_debug_info_obj, theos_mouse_debug_info);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_file_read_obj, theos_file_read);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_file_write_obj, theos_file_write);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_fs_is_dir_obj, theos_fs_is_dir);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_fs_mkdir_obj, theos_fs_mkdir);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_fs_readdir_obj, theos_fs_readdir);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_yield_obj, theos_yield);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_exit_obj, theos_exit);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_fork_obj, theos_fork);
static MP_DEFINE_CONST_FUN_OBJ_3(theos_execve_obj, theos_execve);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_waitpid_obj, theos_waitpid);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_kill_obj, theos_kill);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_power_obj, theos_power);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_shutdown_obj, theos_shutdown);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_sleep_state_obj, theos_sleep_state);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_reboot_obj, theos_reboot);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(theos_map_ex_obj, 6, 6, theos_map_ex);
static MP_DEFINE_CONST_FUN_OBJ_3(theos_map_obj, theos_map);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_unmap_obj, theos_unmap);
static MP_DEFINE_CONST_FUN_OBJ_3(theos_mprotect_obj, theos_mprotect);
static MP_DEFINE_CONST_FUN_OBJ_4(theos_thread_create_ex_obj, theos_thread_create_ex);
static MP_DEFINE_CONST_FUN_OBJ_3(theos_thread_create_obj, theos_thread_create);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_thread_join_obj, theos_thread_join);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_thread_exit_obj, theos_thread_exit);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_thread_self_obj, theos_thread_self);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_thread_set_fsbase_obj, theos_thread_set_fsbase);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_thread_get_fsbase_obj, theos_thread_get_fsbase);
static MP_DEFINE_CONST_FUN_OBJ_0(theos_ipc_pipe_obj, theos_ipc_pipe);
static MP_DEFINE_CONST_FUN_OBJ_4(theos_ipc_futex_obj, theos_ipc_futex);
static MP_DEFINE_CONST_FUN_OBJ_3(theos_ipc_shmget_obj, theos_ipc_shmget);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_ipc_shmat_obj, theos_ipc_shmat);
static MP_DEFINE_CONST_FUN_OBJ_1(theos_ipc_shmdt_obj, theos_ipc_shmdt);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_ipc_shmctl_obj, theos_ipc_shmctl);
static MP_DEFINE_CONST_FUN_OBJ_2(theos_ipc_msgget_obj, theos_ipc_msgget);
static MP_DEFINE_CONST_FUN_OBJ_4(theos_ipc_msgsnd_obj, theos_ipc_msgsnd);
static MP_DEFINE_CONST_FUN_OBJ_5(theos_ipc_msgrcv_obj, theos_ipc_msgrcv);

static const mp_rom_map_elem_t theos_user_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_time_ticks_ms), MP_ROM_PTR(&theos_time_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_time_sleep_ms), MP_ROM_PTR(&theos_time_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_time_rtc_get), MP_ROM_PTR(&theos_time_rtc_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_cpu_info), MP_ROM_PTR(&theos_cpu_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_sched_info), MP_ROM_PTR(&theos_sched_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_ahci_irq_info), MP_ROM_PTR(&theos_ahci_irq_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_rcu_sync), MP_ROM_PTR(&theos_rcu_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_rcu_info), MP_ROM_PTR(&theos_rcu_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_proc_snapshot), MP_ROM_PTR(&theos_proc_snapshot_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_write), MP_ROM_PTR(&theos_console_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_kdebug_write), MP_ROM_PTR(&theos_kdebug_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_set), MP_ROM_PTR(&theos_console_route_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_read), MP_ROM_PTR(&theos_console_route_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_set_sid), MP_ROM_PTR(&theos_console_route_set_sid_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_read_sid), MP_ROM_PTR(&theos_console_route_read_sid_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_input_write_sid), MP_ROM_PTR(&theos_console_route_input_write_sid_obj) },
    { MP_ROM_QSTR(MP_QSTR_console_route_input_read), MP_ROM_PTR(&theos_console_route_input_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_kbd_get_scancode), MP_ROM_PTR(&theos_kbd_get_scancode_obj) },
    { MP_ROM_QSTR(MP_QSTR_kbd_inject_scancode), MP_ROM_PTR(&theos_kbd_inject_scancode_obj) },
    { MP_ROM_QSTR(MP_QSTR_kbd_capture_set), MP_ROM_PTR(&theos_kbd_capture_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_get_event), MP_ROM_PTR(&theos_mouse_get_event_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse_debug_info), MP_ROM_PTR(&theos_mouse_debug_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_file_read), MP_ROM_PTR(&theos_file_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_file_write), MP_ROM_PTR(&theos_file_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_fs_is_dir), MP_ROM_PTR(&theos_fs_is_dir_obj) },
    { MP_ROM_QSTR(MP_QSTR_fs_mkdir), MP_ROM_PTR(&theos_fs_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_fs_readdir), MP_ROM_PTR(&theos_fs_readdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_yield), MP_ROM_PTR(&theos_yield_obj) },
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&theos_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_fork), MP_ROM_PTR(&theos_fork_obj) },
    { MP_ROM_QSTR(MP_QSTR_execve), MP_ROM_PTR(&theos_execve_obj) },
    { MP_ROM_QSTR(MP_QSTR_waitpid), MP_ROM_PTR(&theos_waitpid_obj) },
    { MP_ROM_QSTR(MP_QSTR_kill), MP_ROM_PTR(&theos_kill_obj) },
    { MP_ROM_QSTR(MP_QSTR_power), MP_ROM_PTR(&theos_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&theos_shutdown_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_state), MP_ROM_PTR(&theos_sleep_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_reboot), MP_ROM_PTR(&theos_reboot_obj) },
    { MP_ROM_QSTR(MP_QSTR_map_ex), MP_ROM_PTR(&theos_map_ex_obj) },
    { MP_ROM_QSTR(MP_QSTR_map), MP_ROM_PTR(&theos_map_obj) },
    { MP_ROM_QSTR(MP_QSTR_unmap), MP_ROM_PTR(&theos_unmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_mprotect), MP_ROM_PTR(&theos_mprotect_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_create_ex), MP_ROM_PTR(&theos_thread_create_ex_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_create), MP_ROM_PTR(&theos_thread_create_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_join), MP_ROM_PTR(&theos_thread_join_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_exit), MP_ROM_PTR(&theos_thread_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_self), MP_ROM_PTR(&theos_thread_self_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_set_fsbase), MP_ROM_PTR(&theos_thread_set_fsbase_obj) },
    { MP_ROM_QSTR(MP_QSTR_thread_get_fsbase), MP_ROM_PTR(&theos_thread_get_fsbase_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_pipe), MP_ROM_PTR(&theos_ipc_pipe_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_futex), MP_ROM_PTR(&theos_ipc_futex_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_shmget), MP_ROM_PTR(&theos_ipc_shmget_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_shmat), MP_ROM_PTR(&theos_ipc_shmat_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_shmdt), MP_ROM_PTR(&theos_ipc_shmdt_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_shmctl), MP_ROM_PTR(&theos_ipc_shmctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_msgget), MP_ROM_PTR(&theos_ipc_msgget_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_msgsnd), MP_ROM_PTR(&theos_ipc_msgsnd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipc_msgrcv), MP_ROM_PTR(&theos_ipc_msgrcv_obj) },
};
static MP_DEFINE_CONST_DICT(theos_user_module_globals, theos_user_module_globals_table);

const mp_obj_module_t theos_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&theos_user_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_theos, theos_user_cmodule);
