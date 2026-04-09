#!/usr/bin/env python3
import argparse
import re
import statistics
from pathlib import Path

KEYVAL_RE = re.compile(r"([A-Za-z0-9_]+)=(-?[0-9]+)")


def parse_keyvals(line: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for key, value in KEYVAL_RE.findall(line):
        out[key] = int(value)
    return out


def stat_line(rows: list[dict[str, int]], key: str, scale: float = 1.0, precision: int = 2) -> str | None:
    vals = [row[key] for row in rows if key in row]
    if not vals:
        return None
    avg = statistics.fmean(vals) / scale
    mx = max(vals) / scale
    p95 = statistics.quantiles(vals, n=20)[18] / scale if len(vals) >= 20 else mx
    return f"{key}: avg={avg:.{precision}f} p95={p95:.{precision}f} max={mx:.{precision}f}"


def print_section(title: str, rows: list[dict[str, int]], lines: list[str]) -> None:
    print(f"\n[{title}] windows={len(rows)}")
    if not rows:
        print("  no profiling lines found")
        return

    last = rows[-1]
    last_preview = " ".join(f"{k}={v}" for k, v in sorted(last.items()))
    print(f"  last: {last_preview}")

    interesting = [
        ("bytes_s", 1.0, 2),
        ("calls_s", 1.0, 2),
        ("stall_total_ms", 1.0, 2),
        ("stall_max_ms", 1.0, 2),
        ("buffered_peak_ms", 1.0, 2),
        ("fps_x10", 10.0, 2),
        ("present_avg_ms_x10", 10.0, 2),
        ("present_max_ms", 1.0, 2),
        ("frame_gap_max_ms", 1.0, 2),
        ("req_bps", 1.0, 2),
        ("wrote_bps", 1.0, 2),
        ("submit_gap_max_ms", 1.0, 2),
        ("mix_avg_ms_x10", 10.0, 2),
        ("mix_max_ms", 1.0, 2),
    ]
    for key, scale, precision in interesting:
        line = stat_line(rows, key, scale=scale, precision=precision)
        if line:
            print(f"  {line}")

    if lines:
        print("  raw lines:")
        for raw in lines[-5:]:
            print(f"    {raw.rstrip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize TheOS DOOM profiling logs.")
    parser.add_argument("log", nargs="?", default="Build/serial.log", help="Path to serial log")
    args = parser.parse_args()

    log_path = Path(args.log)
    if not log_path.is_file():
        print(f"error: log not found: {log_path}")
        return 1

    hda_rows: list[dict[str, int]] = []
    video_rows: list[dict[str, int]] = []
    audio_rows: list[dict[str, int]] = []
    hda_lines: list[str] = []
    video_lines: list[str] = []
    audio_lines: list[str] = []
    hda_stalls = 0
    drm_atomic_fail = 0
    snd_missed = 0

    for line in log_path.read_text(errors="replace").splitlines():
        if "[HDA-PROF]" in line:
            hda_rows.append(parse_keyvals(line))
            hda_lines.append(line)
        if "[DOOM-PROF][VIDEO]" in line:
            video_rows.append(parse_keyvals(line))
            video_lines.append(line)
        if "[DOOM-PROF][AUDIO]" in line:
            audio_rows.append(parse_keyvals(line))
            audio_lines.append(line)
        if "[HDA] stream stalled" in line:
            hda_stalls += 1
        if "[DOOM] DRM atomic commit failed" in line:
            drm_atomic_fail += 1
        if "I_SoundUpdate: missed 10 buffer writes" in line:
            snd_missed += 1

    print(f"log: {log_path}")
    print(f"events: hda_stall_lines={hda_stalls} drm_atomic_fail={drm_atomic_fail} snd_miss_bursts={snd_missed}")

    print_section("HDA", hda_rows, hda_lines)
    print_section("DOOM VIDEO", video_rows, video_lines)
    print_section("DOOM AUDIO", audio_rows, audio_lines)

    if not hda_rows and not video_rows and not audio_rows:
        print("\nno profiling payload found. make sure the instrumented kernel/userland was booted.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
