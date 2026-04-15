import theos


def tick_ms():
    return int(theos.time_ticks_ms())


def sleep_ms(ms):
    theos.time_sleep_ms(int(ms))


def yield_now():
    theos.sched_yield()


def cpu_info():
    return theos.cpu_info()


def sched_info():
    return theos.sched_info()


def rcu_info():
    return theos.rcu_info()


def ahci_info():
    return theos.ahci_info()


def proc_snapshot(max_entries=32):
    return theos.proc_snapshot(int(max_entries))


def net_raw_stats(path="/dev/net0"):
    return theos.net_raw_stats(path)


def stress_scheduler(workers, duration_ms):
    return theos.stress_scheduler(int(workers), int(duration_ms))


def ws_connect(role=None):
    # WindowServer socket can appear a bit later than shell readiness.
    # Retry briefly to avoid transient ENOENT/OSError(2) at startup.
    retries = 200
    for _ in range(retries):
        try:
            if role is None:
                return theos.window_connect()
            return theos.window_connect(int(role))
        except OSError:
            theos.time_sleep_ms(20)
    if role is None:
        return theos.window_connect()
    return theos.window_connect(int(role))


def ws_disconnect():
    return theos.window_disconnect()


def ws_create(title, width, height, x, y):
    return int(theos.window_create(title, int(width), int(height), int(x), int(y)))


def ws_set_text(window_id, text):
    theos.window_set_text(int(window_id), text)


def ws_poll_event():
    evt = theos.window_poll_event()
    if evt is None:
        return None
    return {"type": int(evt[0]), "window_id": int(evt[1]), "key": int(evt[2])}
