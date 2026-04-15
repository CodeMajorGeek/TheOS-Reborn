def _to_int(value, default=0):
    try:
        return int(value)
    except Exception:
        return int(default)


def _get_int(obj, key, default=0):
    try:
        if hasattr(obj, "get"):
            return _to_int(obj.get(key, default), default)
    except Exception:
        pass
    return _to_int(default, default)


def compute_health(prev_snap, snap):
    score = 100
    notes = []

    sched_total_rq = _get_int(snap.get("sched", {}), "total_rq_depth", 0)
    rcu_pending = _get_int(snap.get("rcu", {}), "callbacks_pending", 0)
    net_rx_drop = _get_int(snap.get("net", {}), "rx_dropped", 0)
    net_tx_drop = _get_int(snap.get("net", {}), "tx_dropped", 0)

    if sched_total_rq > 24:
        score -= 20
        notes.append("runqueue high")
    if rcu_pending > 256:
        score -= 20
        notes.append("rcu backlog")
    if net_rx_drop > 0 or net_tx_drop > 0:
        score -= 10
        notes.append("net drops")

    if prev_snap is not None:
        d_tick = _to_int(snap.get("tick_ms", 0), 0) - _to_int(prev_snap.get("tick_ms", 0), 0)
        if d_tick > 0:
            d_exec = _get_int(snap.get("cpu", {}), "sched_exec_total", 0) - _get_int(prev_snap.get("cpu", {}), "sched_exec_total", 0)
            d_idle = _get_int(snap.get("cpu", {}), "sched_idle_hlt_total", 0) - _get_int(prev_snap.get("cpu", {}), "sched_idle_hlt_total", 0)
            den = d_exec + d_idle
            usage = int((100 * d_exec) / den) if den > 0 else 0
            if usage > 95:
                score -= 10
                notes.append("cpu saturated")
        else:
            usage = 0
    else:
        usage = 0

    if score < 0:
        score = 0
    return {"score": score, "notes": notes, "cpu_usage_pct": usage}
