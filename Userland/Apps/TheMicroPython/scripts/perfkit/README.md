# PerfKit

PerfKit est une application MicroPython de debug/perf pour TheOS.

## Lancement

- Depuis TheShell:
  - `MicroPython /system/python/perfwm.py`
  - `MicroPython /system/python/perfwm.py --mode stress`
  - `MicroPython /system/python/perfwm.py --snapshot`

## Modes

- `live`: monitoring continu (CPU/scheduler/RCU/proc/net).
- `stress`: lance un stress scheduler natif via `theos.stress_scheduler()`.
- `snapshot`: imprime un snapshot unique et quitte.

## API native utilisée

Le module natif `theos` expose:

- métriques: `cpu_info`, `sched_info`, `rcu_info`, `ahci_info`, `proc_snapshot`
- timing: `time_ticks_ms`, `time_sleep_ms`, `sched_yield`
- console: `console_set_sid`, `console_read_sid`, `console_input_write_sid`
- window: `window_connect`, `window_create`, `window_set_text`, `window_poll_event`, `window_disconnect`
- réseau: `net_raw_stats`
- stress: `stress_scheduler`
