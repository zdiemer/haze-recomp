"""Summarize a native renderer CSV after visually verifying the sampled scene.

Example: python scripts/diagnose.py renderer local/runs/<run>/renderer.csv --last 600
CPU scopes overlap: vertex/hash/decode costs are included in draw preparation.
"""
import argparse
import csv
import json
import statistics
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(path, start_frame=0, last=600, min_draws=500, end_frame=None):
    rows = []
    with Path(path).open(newline="") as stream:
        for raw in csv.DictReader(stream):
            try:
                row = {k: float(v) for k, v in raw.items()}
            except (TypeError, ValueError):
                continue  # A running process may have a partially buffered last row.
            if (row["frame"] >= start_frame and row["draws"] >= min_draws
                    and (end_frame is None or row["frame"] <= end_frame)):
                rows.append(row)
    rows = rows[-last:] if last else rows
    if not rows:
        raise ValueError("No complete frames match the scene filter")
    intervals = [r["dt_ms"] for r in rows]
    result = {
        "source": str(path), "frames": len(rows),
        "first_frame": int(rows[0]["frame"]), "last_frame": int(rows[-1]["frame"]),
        "fps": 1000 / statistics.mean(intervals),
        "frame_ms": {"mean": statistics.mean(intervals),
                     "p50": statistics.median(intervals),
                     "p95": percentile(intervals, .95),
                     "p99": percentile(intervals, .99), "max": max(intervals)},
        "frames_over_33_333_ms": sum(t > 1000 / 30 for t in intervals),
        "frames_over_50_ms": sum(t > 50 for t in intervals),
        "mean_ms": {k: statistics.mean(r[k] for r in rows)
                    for k in rows[0] if k.endswith("_ms") and k != "dt_ms"},
        "mean_draws": statistics.mean(r["draws"] for r in rows),
        "frames_with_shader_compilation": sum(
            r["vs_compile_ms"] > 0 or r["ps_compile_ms"] > 0 for r in rows),
        "texture_evictions": sum(int(r["evictions"]) for r in rows),
        "note": "Scene must be verified visually. CPU scopes overlap; do not add them.",
    }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--end-frame", type=int)
    parser.add_argument("--last", type=int, default=600, help="0 uses all matching frames")
    parser.add_argument("--min-draws", type=int, default=500)
    args = parser.parse_args()
    if args.last < 0 or args.start_frame < 0:
        parser.error("Frame filters must be nonnegative")
    if args.end_frame is not None and args.end_frame < args.start_frame:
        parser.error("End frame must be at least the start frame")
    print(json.dumps(summarize(args.csv, args.start_frame, args.last,
                               args.min_draws, args.end_frame), indent=2))
