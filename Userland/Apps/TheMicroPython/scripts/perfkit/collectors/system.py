try:
    from perfkit.collectors import theos_adapter as th
except Exception:
    from collectors import theos_adapter as th


def _to_int(value, default=0):
    try:
        return int(value)
    except Exception:
        return int(default)


def _to_bool(value, default=False):
    try:
        return bool(value)
    except Exception:
        return bool(default)


def _to_str(value, default=""):
    try:
        return str(value)
    except Exception:
        return str(default)


def _normalize_dict(raw, schema):
    out = {}
    for key, default in schema.items():
        value = default
        try:
            if hasattr(raw, "get"):
                value = raw.get(key, default)
        except Exception:
            value = default

        if isinstance(default, bool):
            out[key] = _to_bool(value, default)
        elif isinstance(default, int):
            out[key] = _to_int(value, default)
        else:
            out[key] = _to_str(value, default)
    return out


def collect_snapshot():
    cpu_raw = th.cpu_info()
    sched_raw = th.sched_info()
    rcu_raw = th.rcu_info()
    ahci_raw = th.ahci_info()
    proc_raw = th.proc_snapshot(32)

    try:
        net_raw = th.net_raw_stats("/dev/net0")
    except OSError:
        net_raw = {
            "rx_packets": 0,
            "rx_dropped": 0,
            "tx_packets": 0,
            "tx_dropped": 0,
            "irq_count": 0,
            "rx_queue_depth": 0,
            "rx_queue_capacity": 0,
            "link_up": False,
            "irq_mode": 0,
            "mac_hex": "00:00:00:00:00:00",
            "error": "net_raw_stats_unavailable",
        }

    cpu = _normalize_dict(cpu_raw, {
        "cpu_index": 0,
        "apic_id": 0,
        "online_cpus": 0,
        "tick_hz": 0,
        "ticks": 0,
        "sched_exec_total": 0,
        "sched_idle_hlt_total": 0,
    })
    sched = _normalize_dict(sched_raw, {
        "current_cpu": 0,
        "local_rq_depth": 0,
        "total_rq_depth": 0,
    })
    rcu = _normalize_dict(rcu_raw, {
        "gp_seq": 0,
        "gp_target": 0,
        "callbacks_pending": 0,
        "local_read_depth": 0,
        "local_preempt_count": 0,
    })
    ahci = _normalize_dict(ahci_raw, {
        "mode": 0,
        "count": 0,
    })
    proc = _normalize_dict(proc_raw, {
        "total": 0,
        "visible": 0,
    })
    net = _normalize_dict(net_raw, {
        "rx_packets": 0,
        "rx_dropped": 0,
        "tx_packets": 0,
        "tx_dropped": 0,
        "irq_count": 0,
        "rx_queue_depth": 0,
        "rx_queue_capacity": 0,
        "link_up": False,
        "irq_mode": 0,
        "mac_hex": "00:00:00:00:00:00",
        "error": "",
    })

    return {
        "tick_ms": _to_int(th.tick_ms()),
        "cpu": cpu,
        "sched": sched,
        "rcu": rcu,
        "ahci": ahci,
        "proc": proc,
        "net": net,
    }
