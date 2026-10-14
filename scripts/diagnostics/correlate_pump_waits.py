"""Correlate clipped PPU service-call intervals with successful pump presents.

Wait records are a lower bound: the recorder omits calls below 200 us and
per-thread files may not have been flushed after an abnormal process exit.
These overlaps describe timing, not a proven causal critical path.
"""
import argparse
import bisect
import collections
import csv
import json
from pathlib import Path

from .summarize_waits import load_names, union_ticks
from .summarize_pump import read_pump as _read_pump


PUMP_TIME_FIELDS = ("dt_ms", "wait_ms", "fifo_ms", "present_ms", "tick_ms", "other_ms")
WAIT_FILE_FIELDS = ("begin_qpc", "end_qpc", "frequency", "host_tid", "guest_tid",
                    "kind", "id", "lr", "arg0")


def _read_waits(directory, frequency, guest_tid):
    paths = sorted(Path(directory).glob("wait-*.csv"))
    if not paths:
        raise ValueError("No PPU wait streams")
    waits = []
    for path in paths:
        with path.open(newline="") as stream:
            for raw in csv.DictReader(stream):
                try:
                    row = {key: raw[key] for key in WAIT_FILE_FIELDS}
                    row["begin_qpc"] = int(row["begin_qpc"])
                    row["end_qpc"] = int(row["end_qpc"])
                    row["frequency"] = int(row["frequency"])
                    row["host_tid"] = int(row["host_tid"])
                    row["guest_tid"] = int(row["guest_tid"])
                except (KeyError, TypeError, ValueError) as error:
                    raise ValueError(f"Invalid PPU wait row in {path.name}") from error
                if row["frequency"] != frequency:
                    raise ValueError("PPU wait QPC frequency mismatch")
                if row["end_qpc"] <= row["begin_qpc"]:
                    raise ValueError("Invalid PPU wait interval")
                if guest_tid is None or row["guest_tid"] == guest_tid:
                    waits.append(row)
    return waits


def _caller_key(wait):
    # Keep syscall and HLE namespaces distinct even when their numeric IDs
    # happen to match. The caller PC and first argument distinguish call sites.
    return (wait["guest_tid"], wait["kind"], wait["id"].upper(),
            wait["lr"].upper(), wait["arg0"].upper())


def correlate(pump_path, wait_directory, start=None, end=None, guest_tid=1):
    pumps = _read_pump(pump_path)
    frequency = pumps[0]["qpc_frequency"]
    if start is not None:
        if end is None or end <= start:
            raise ValueError("Invalid requested QPC window")
        if pumps[0]["begin_qpc"] > start or pumps[-1]["end_qpc"] < end:
            raise ValueError("Pump ring does not cover requested window")
        selected = [row for row in pumps
                    if start <= row["begin_qpc"] < row["end_qpc"] <= end]
    else:
        selected = pumps
    if not selected:
        raise ValueError("No full pump intervals in requested window")

    waits = _read_waits(wait_directory, frequency, guest_tid)
    ends = [row["end_qpc"] for row in selected]
    per_frame = []
    for pump in selected:
        per_frame.append({"pump": pump,
                          "host_intervals": collections.defaultdict(list),
                          "groups": collections.defaultdict(list),
                          "calls": 0})

    for wait in waits:
        i = bisect.bisect_right(ends, wait["begin_qpc"])
        while i < len(selected) and selected[i]["begin_qpc"] < wait["end_qpc"]:
            pump = selected[i]
            begin = max(wait["begin_qpc"], pump["begin_qpc"])
            finish = min(wait["end_qpc"], pump["end_qpc"])
            if begin < finish:
                duration = finish - begin
                frame = per_frame[i]
                frame["host_intervals"][wait["host_tid"]].append((begin, finish))
                frame["groups"][_caller_key(wait)].append(duration)
                frame["calls"] += 1
            i += 1

    names = load_names(Path(__file__).resolve().parents[2])

    def format_caller(key, calls, ticks, max_ticks):
        tid, kind, ident, lr, arg0 = key
        name = names.get(kind, {}).get(ident, ident)
        return {"guest_tid": tid, "kind": kind, "id": ident, "name": name,
                "lr": lr, "arg0": arg0, "overlap_calls": calls,
                "wall_ms": ticks * 1000 / frequency,
                "max_clipped_ms": max_ticks * 1000 / frequency}

    frame_rows = []
    for frame in per_frame:
        pump = frame["pump"]
        host_union_ticks = {
            host: union_ticks(intervals)
            for host, intervals in frame["host_intervals"].items()
        }
        wait_union = sum(host_union_ticks.values()) * 1000 / frequency
        callers = [format_caller(key, len(durations), sum(durations), max(durations))
                   for key, durations in frame["groups"].items()]
        callers.sort(key=lambda row: row["wall_ms"], reverse=True)
        frame_rows.append({
            "frame": pump["frame"], "flips": pump["flips"],
            "begin_qpc": pump["begin_qpc"], "end_qpc": pump["end_qpc"],
            "dt_ms": pump["dt_ms"],
            "pump": {key: pump[key] for key in PUMP_TIME_FIELDS[1:]},
            "pump_counts": {key: pump[key] for key in ("waits", "queued_waits", "drains")},
            "thread_cpu_ms": pump["thread_cpu_ms"] if pump["cpu_valid"] else None,
            "wait_union_ms": wait_union,
            "wait_union_by_host_tid_ms": {
                str(host): ticks * 1000 / frequency
                for host, ticks in sorted(host_union_ticks.items())},
            "overlap_calls": frame["calls"], "callers": callers,
        })

    def summarize_group(group_rows):
        count = len(group_rows)
        result = {"frames": count}
        if not count:
            return result
        keys = ("dt_ms", "wait_ms", "fifo_ms", "present_ms", "tick_ms", "other_ms",
                "wait_union_ms", "thread_cpu_ms")
        def frame_value(row, key):
            if key in PUMP_TIME_FIELDS[1:]:
                return row["pump"][key]
            return row[key]
        means = {}
        for key in keys:
            values = [frame_value(row, key) for row in group_rows]
            values = [value for value in values if value is not None]
            means[key] = sum(values) / len(values) if values else None
        result["means_ms"] = means
        result["wait_calls_mean"] = sum(r["overlap_calls"] for r in group_rows) / count
        result["callers"] = top_callers(group_rows)
        return result

    def top_callers(group_rows, limit=10):
        grouped = collections.defaultdict(lambda: {"calls": 0, "ticks": 0,
                                                    "max_call_ticks": 0})
        for row in group_rows:
            for caller in row["callers"]:
                key = (caller["guest_tid"], caller["kind"], caller["id"],
                       caller["lr"], caller["arg0"])
                target = grouped[key]
                target["calls"] += caller["overlap_calls"]
                # Stored in ms here; reconstructing integer ticks is unnecessary
                # for aggregation, and precision is retained by QPC scaling.
                target["ticks"] += caller["wall_ms"] * frequency / 1000
                target["max_call_ticks"] = max(target["max_call_ticks"],
                                                caller["max_clipped_ms"] * frequency / 1000)
        result = [format_caller(key, data["calls"], round(data["ticks"]),
                                round(data["max_call_ticks"]))
                  for key, data in grouped.items()]
        return sorted(result, key=lambda row: row["wall_ms"], reverse=True)[:limit]

    slow = []
    for row in sorted(frame_rows, key=lambda row: row["dt_ms"], reverse=True)[:10]:
        slow.append({**row, "callers": row["callers"][:10]})
    return {
        "pump_source": str(pump_path), "wait_source": str(wait_directory),
        "guest_tid": guest_tid,
        "note": ("PPU service intervals are clipped to pump-present QPC intervals. "
                 "Per-host-thread unions avoid nested-scope double counting; sums "
                 "across host threads may exceed wall time. Caller groups overlap. "
                 "Wait coverage is a lower bound (calls below 200us are omitted, "
                 "and per-thread streams may not have flushed). Overlap is not a "
                 "causal critical-path attribution."),
        "all": summarize_group(frame_rows),
        "over_50_ms": summarize_group([r for r in frame_rows if r["dt_ms"] > 50]),
        "over_100_ms": summarize_group([r for r in frame_rows if r["dt_ms"] > 100]),
        "slowest": slow,
    }


def summarize_windows(pump_path, wait_directory, windows_path, guest_tid=1):
    manifest = json.loads(Path(windows_path).read_text())
    if "qpc_frequency" in manifest:
        pumps = _read_pump(pump_path)
        if int(manifest["qpc_frequency"]) != pumps[0]["qpc_frequency"]:
            raise ValueError("Present window QPC frequency mismatch")
    blocks = []
    for window in manifest["blocks"]:
        result = correlate(pump_path, wait_directory, window["start_qpc"],
                           window["end_qpc"], guest_tid)
        blocks.append({"flags": window.get("flags"), **result})
    return {"blocks": blocks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pump_csv")
    parser.add_argument("wait_directory")
    parser.add_argument("--windows", help="Present-mode manifest with QPC block bounds")
    parser.add_argument("--guest-tid", type=int, default=1)
    parser.add_argument("--output")
    args = parser.parse_args()
    result = (summarize_windows(args.pump_csv, args.wait_directory,
                                args.windows, args.guest_tid)
              if args.windows else
              correlate(args.pump_csv, args.wait_directory, guest_tid=args.guest_tid))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
