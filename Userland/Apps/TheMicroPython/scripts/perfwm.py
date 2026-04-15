import sys


def _log(msg):
    line_nl = "[perfwm] " + str(msg) + "\n"
    try:
        print(line_nl.rstrip("\n"))
    except Exception:
        pass

def _kdebug_exception(exc):
    """Envoie traceback complet vers kdebug (sys.print_exception ne le fait pas sans MICROPY_PY_IO)."""
    try:
        import theos
        theos.kdebug_write("[perfwm] Python exception:\n")
        theos.kdebug_print_exception(exc)
    except Exception:
        pass


def _bootstrap_imports():
    if not hasattr(sys, "path"):
        return
    # Some MicroPython builds expose sys.path with limited operators.
    # Avoid "in" checks and append defensively for maximum compatibility.
    for p in ("/system/python", "/system/python/scripts", "/system/python/perfkit", "/system"):
        try:
            sys.path.append(p)
        except Exception:
            pass


def _load_main():
    # Try package import first.
    try:
        pkg = __import__("perfkit.app", None, None, ("main",), 0)
        return pkg.main
    except Exception:
        pass

    # Then flat import with /system/python/perfkit in sys.path.
    mod = __import__("app")
    return mod.main


def _main():
    _log("bootstrap start")
    _bootstrap_imports()
    _log("bootstrap done")
    try:
        _log("load main")
        perfkit_main = _load_main()
        _log("main loaded")
        perfkit_main()
        _log("main exited")
        return 0
    except Exception as e:
        _log("import/runtime error")
        _kdebug_exception(e)
        try:
            if hasattr(sys, "print_exception"):
                sys.print_exception(e)
            else:
                print(e)
        except Exception:
            pass
        if hasattr(sys, "path"):
            try:
                _log("sys.path:")
                print(sys.path)
            except Exception:
                pass
        return 1


_main()
