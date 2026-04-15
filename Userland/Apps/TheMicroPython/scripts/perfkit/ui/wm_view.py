try:
    from perfkit.core.ringbuffer import RingBuffer
except Exception:
    from core.ringbuffer import RingBuffer


def _to_int(value, default=0):
    try:
        return int(value)
    except Exception:
        return int(default)


def _to_str(value, default=""):
    try:
        return str(value)
    except Exception:
        return str(default)


def _get_int(obj, key, default=0):
    try:
        if hasattr(obj, "get"):
            return _to_int(obj.get(key, default), default)
    except Exception:
        pass
    return _to_int(default, default)


def _get_str(obj, key, default=""):
    try:
        if hasattr(obj, "get"):
            return _to_str(obj.get(key, default), default)
    except Exception:
        pass
    return _to_str(default, default)

def _s(*parts):
    out = ""
    for p in parts:
        out += str(p)
    return out


class WMView:
    def __init__(self):
        self.timeline = RingBuffer(20)

    def push_event(self, text):
        self.timeline.push(text)

    def render_text(self, snap, health, mode):
        cpu = snap.get("cpu", {})
        sched = snap.get("sched", {})
        rcu = snap.get("rcu", {})
        ahci = snap.get("ahci", {})
        net = snap.get("net", {})
        proc = snap.get("proc", {})

        lines = []
        lines.append("TheOS PerfKit WM")
        lines.append(
            _s(
                "mode=",
                _to_str(mode, "live"),
                " tick=",
                _to_int(snap.get("tick_ms", 0), 0),
                " score=",
                _to_int(health.get("score", 0), 0),
                " cpu_usage=",
                _to_int(health.get("cpu_usage_pct", 0), 0),
                "%",
            )
        )
        lines.append("")
        lines.append("System")
        lines.append(_s("  cpu_index=", _get_int(cpu, "cpu_index", 0), " apic=", _get_int(cpu, "apic_id", 0), " online=", _get_int(cpu, "online_cpus", 0), " tick_hz=", _get_int(cpu, "tick_hz", 0)))
        lines.append(_s("  sched_rq local=", _get_int(sched, "local_rq_depth", 0), " total=", _get_int(sched, "total_rq_depth", 0), " preempt=", _get_int(sched, "current_cpu", 0)))
        lines.append(_s("  rcu callbacks=", _get_int(rcu, "callbacks_pending", 0), " gp=", _get_int(rcu, "gp_seq", 0), "->", _get_int(rcu, "gp_target", 0)))
        lines.append(_s("  ahci irq_count=", _get_int(ahci, "count", 0), " mode=", _get_int(ahci, "mode", 0)))
        lines.append(_s("  net rx=", _get_int(net, "rx_packets", 0), " tx=", _get_int(net, "tx_packets", 0), " drop_rx=", _get_int(net, "rx_dropped", 0), " drop_tx=", _get_int(net, "tx_dropped", 0), " link=", 1 if bool(net.get("link_up", False)) else 0, " mac=", _get_str(net, "mac_hex", "00:00:00:00:00:00")))
        lines.append("")
        lines.append("Processes")
        lines.append(_s("  visible=", _get_int(proc, "visible", 0), " total=", _get_int(proc, "total", 0)))
        lines.append("")
        lines.append("Alerts")
        notes = health.get("notes", [])
        if notes:
            for note in notes:
                lines.append(_s("  - ", note))
        else:
            lines.append("  - none")
        lines.append("")
        lines.append("Timeline")
        for item in self.timeline.items():
            lines.append(_s("  * ", item))
        return "\n".join(lines)
