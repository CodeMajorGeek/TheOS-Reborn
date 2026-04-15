import sys

try:
    from perfkit.analysis.health import compute_health
    from perfkit.collectors import theos_adapter as th
    from perfkit.collectors.system import collect_snapshot
    from perfkit.core import config
    from perfkit.ui.wm_client import WMClient
    from perfkit.ui.wm_view import WMView
except Exception:
    # Flat-import mode: works if /system/python/perfkit is on sys.path
    from analysis.health import compute_health
    from collectors import theos_adapter as th
    from collectors.system import collect_snapshot
    from core import config
    from ui.wm_client import WMClient
    from ui.wm_view import WMView


def _log(msg):
    line_nl = "[perfkit] " + str(msg) + "\n"
    try:
        print(line_nl.rstrip("\n"))
    except Exception:
        pass

def _kdebug_exception(exc):
    try:
        import theos
        theos.kdebug_write("[perfkit] Python exception:\n")
        theos.kdebug_print_exception(exc)
    except Exception:
        pass

def _s(*parts):
    out = ""
    for p in parts:
        out += str(p)
    return out


def _parse_args(argv):
    mode = config.DEFAULT_MODE
    refresh_ms = config.DEFAULT_REFRESH_MS
    stress_workers = config.DEFAULT_STRESS_WORKERS
    stress_duration_ms = config.DEFAULT_STRESS_DURATION_MS
    snapshot_once = False

    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--mode" and i + 1 < len(argv):
            mode = argv[i + 1]
            i += 2
            continue
        if arg == "--refresh-ms" and i + 1 < len(argv):
            refresh_ms = int(argv[i + 1])
            i += 2
            continue
        if arg == "--stress-workers" and i + 1 < len(argv):
            stress_workers = int(argv[i + 1])
            i += 2
            continue
        if arg == "--stress-ms" and i + 1 < len(argv):
            stress_duration_ms = int(argv[i + 1])
            i += 2
            continue
        if arg == "--snapshot":
            snapshot_once = True
            i += 1
            continue
        if arg == "--help":
            print("Usage: perfwm.py [--mode live|stress] [--refresh-ms N] [--stress-workers N] [--stress-ms N] [--snapshot]")
            return None
        i += 1

    return {
        "mode": mode,
        "refresh_ms": refresh_ms,
        "stress_workers": stress_workers,
        "stress_duration_ms": stress_duration_ms,
        "snapshot_once": snapshot_once,
    }


def _run_stress(view, cfg):
    view.push_event(_s("starting stress workers=", cfg["stress_workers"], " duration=", cfg["stress_duration_ms"], "ms"))
    result = th.stress_scheduler(cfg["stress_workers"], cfg["stress_duration_ms"])
    view.push_event(
        _s(
            "stress ",
            "PASS" if result["pass"] else "FAIL",
            " workers_ok=",
            result["workers_ok"],
            "/",
            result["workers_spawned"],
            " cpu_cov=",
            result["cpu_coverage"],
            " max_rq=",
            result["max_total_rq"],
        )
    )


def main():
    try:
        _log("start")
        cfg = _parse_args(sys.argv)
        if cfg is None:
            _log("help/usage requested")
            return

        wm = WMClient()
        _log("wm connect")
        wm.connect()
        _log("wm connected")
        window_id = wm.create_window(
            config.WINDOW_TITLE,
            config.WINDOW_WIDTH,
            config.WINDOW_HEIGHT,
            config.WINDOW_X,
            config.WINDOW_Y,
        )
        _log(_s("window created id=", int(window_id)))

        view = WMView()
        prev = None
        tick_last = 0
        stress_done = False
        loop_count = 0
        while True:
            loop_count += 1
            try:
                snap = collect_snapshot()
            except OSError:
                wm.sleep_ms(50)
                continue
            except Exception as e:
                _log(_s("collect failed type=", type(e), " err=", e))
                _kdebug_exception(e)
                wm.sleep_ms(50)
                continue

            try:
                health = compute_health(prev, snap)
            except Exception as e:
                _log(_s("health failed type=", type(e), " err=", e))
                _kdebug_exception(e)
                wm.sleep_ms(50)
                continue
            prev = snap

            if cfg["mode"] == "stress" and not stress_done:
                _run_stress(view, cfg)
                stress_done = True

            try:
                tick_now = int(snap.get("tick_ms", 0))
                refresh_ms = int(cfg.get("refresh_ms", config.DEFAULT_REFRESH_MS))
            except Exception as e:
                _log(
                    _s(
                        "timing failed type=",
                        type(e),
                        " err=",
                        e,
                        " tick=",
                        type(snap.get("tick_ms", None)),
                        " refresh=",
                        type(cfg.get("refresh_ms", None)),
                    )
                )
                _kdebug_exception(e)
                wm.sleep_ms(50)
                continue

            if tick_last == 0 or (tick_now - tick_last) >= refresh_ms:
                if health["notes"]:
                    for note in health["notes"]:
                        view.push_event(_s("alert: ", note))
                try:
                    text = view.render_text(snap, health, cfg["mode"])
                except Exception as e:
                    _log(_s("render failed type=", type(e), " err=", e))
                    _kdebug_exception(e)
                    wm.sleep_ms(50)
                    continue
                try:
                    wm.set_text(text)
                except Exception as e:
                    _log(_s("set_text failed type=", type(e), " err=", e))
                    _kdebug_exception(e)
                    wm.sleep_ms(50)
                    continue
                tick_last = tick_now
                if (loop_count % 100) == 0:
                    _log(
                        _s(
                            "heartbeat tick=",
                            tick_now,
                            " mode=",
                            cfg.get("mode", "live"),
                            " score=",
                            int(health.get("score", 0)),
                        )
                    )

            if cfg["snapshot_once"]:
                print(text)
                _log("snapshot printed, exit")
                break

            wm.sleep_ms(20)

        wm.close()
        _log("clean exit")
    except Exception as e:
        _log("fatal error=" + str(e))
        _kdebug_exception(e)

