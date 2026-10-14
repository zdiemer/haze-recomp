"""Summarize successful swapchain presents from normal or profiling builds.

Set RSX_PRESENT_CSV=presents.csv before launch and close the game normally to
flush the fixed ring. Select a visually verified gameplay interval; menus and
loading screens are not representative. Present intervals do not measure input
latency or guarantee that simulation advances at the same rate.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(path, start_frame, end_frame):
    with Path(path).open(newline="") as stream:
        header = stream.readline().strip()
        if not header.startswith("# qpc_frequency="):
            raise ValueError("Expected QPC frequency in the first line")
        frequency = int(header.split("=", 1)[1])
        if frequency <= 0:
            raise ValueError("QPC frequency must be positive")
        rows = [{k: int(v) for k, v in row.items()}
                for row in csv.DictReader(stream)]

    intervals = []
    selected = []
    for previous, current in zip(rows, rows[1:]):
        frame = current["guest_frame"]
        if not start_frame <= frame <= end_frame:
            continue
        if current["present_id"] != previous["present_id"] + 1:
            raise ValueError("Present IDs are not consecutive in the selected interval")
        ticks = current["qpc"] - previous["qpc"]
        if ticks <= 0:
            raise ValueError("Nonpositive present interval")
        intervals.append(ticks * 1000 / frequency)
        selected.append(current)
    if not selected:
        raise ValueError("No complete present intervals match the requested frames")
    return {
        "source": str(path),
        "intervals": len(intervals),
        "first_guest_frame": selected[0]["guest_frame"],
        "last_guest_frame": selected[-1]["guest_frame"],
        "first_present_id": selected[0]["present_id"],
        "last_present_id": selected[-1]["present_id"],
        "fps": 1000 / statistics.mean(intervals),
        "frame_ms": {
            "mean": statistics.mean(intervals),
            "min": min(intervals),
            "p50": statistics.median(intervals),
            "p95": percentile(intervals, .95),
            "p99": percentile(intervals, .99),
            "max": max(intervals),
        },
        "frames_over_33_333_ms": sum(t > 1000 / 30 for t in intervals),
        "frames_under_33_333_ms": sum(t < 1000 / 30 for t in intervals),
        "frames_over_50_ms": sum(t > 50 for t in intervals),
        "note": __doc__,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--start-frame", type=int, required=True)
    parser.add_argument("--end-frame", type=int, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.start_frame < 0 or args.end_frame < args.start_frame:
        parser.error("Use nonnegative frame bounds in increasing order")
    report = json.dumps(summarize(args.csv, args.start_frame, args.end_frame), indent=2)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)
