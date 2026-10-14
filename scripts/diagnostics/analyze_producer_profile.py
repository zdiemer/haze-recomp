"""Validate and summarize bounded Haze producer RAII timing spans.

QPC values define wall intervals; QueryThreadCycleTime deltas are raw counts,
not CPU time or a conversion to milliseconds. Nested spans are inclusive, so
aggregate covered time is calculated as interval unions rather than sums.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path


METADATA_FIELDS = {
    "qpc_frequency", "begun", "completed", "inflight_at_dump", "overwritten",
    "overwritten_inflight", "late_discarded", "max_discarded_end_qpc",
    "discarded_invalid_qpc_observations",
    "earliest_overwritten_inflight_begin_qpc", "capacity", "retained_rows",
    "retained_completed_rows", "retained_inflight_rows", "retained_first_id",
    "retained_last_id", "retained_first_begin_qpc", "dump_qpc", "dump_qpc_valid",
}
ROW_FIELDS = {
    "kind", "id", "parent_id", "generation", "guest_ea", "callsite_lr", "callee_ea",
    "host_tid", "qpc_begin", "qpc_end", "qpc_valid", "cycles_delta", "cycles_valid", "completed",
}


def _uint(text: str, field: str) -> int:
    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text, 10)
    except (AttributeError, ValueError) as exc:
        raise ValueError(f"invalid unsigned integer in {field}: {text!r}") from exc


def _metadata(line: str) -> dict[str, int]:
    if not line.startswith("#"):
        raise ValueError("missing producer-profile metadata")
    values = next(csv.reader([line[1:].strip()]))
    if len(values) % 2 or len(values[::2]) != len(set(values[::2])):
        raise ValueError("malformed or duplicate producer-profile metadata")
    result = {key: _uint(value, key) for key, value in zip(values[::2], values[1::2])}
    missing = METADATA_FIELDS - result.keys()
    if missing:
        raise ValueError(f"missing producer-profile metadata fields: {sorted(missing)}")
    if result["qpc_frequency"] <= 0 or result["capacity"] <= 0:
        raise ValueError("invalid producer-profile frequency or capacity")
    if any(value < 0 for key, value in result.items() if key != "qpc_frequency"):
        raise ValueError("negative producer-profile metadata")
    if result["dump_qpc_valid"] not in (0, 1):
        raise ValueError("invalid dump QPC validity flag")
    return result


def _read_capture(path: Path) -> tuple[dict[str, int], list[dict]]:
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        meta = _metadata(stream.readline())
        reader = csv.DictReader(stream)
        if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("missing or duplicate producer-profile CSV header")
        missing = ROW_FIELDS - set(reader.fieldnames)
        if missing:
            raise ValueError(f"missing producer-profile columns: {sorted(missing)}")
        rows = []
        for line, raw in enumerate(reader, 3):
            if None in raw or any(value is None for value in raw.values()):
                raise ValueError(f"nonrectangular producer-profile row at line {line}")
            row = {key: _uint(value, key) for key, value in raw.items()}
            rows.append(row)
    return meta, rows


def _validate(meta: dict[str, int], rows: list[dict]) -> tuple[dict[int, dict], dict]:
    begun, capacity = meta["begun"], meta["capacity"]
    retained_count = min(begun, capacity)
    expected_overwritten = max(0, begun - capacity)
    if meta["overwritten"] != expected_overwritten:
        raise ValueError("producer-profile overwritten count disagrees with capacity")
    if len(rows) != meta["retained_rows"] or len(rows) != retained_count:
        raise ValueError("retained row count disagrees with metadata/capacity")
    expected_first = begun - retained_count + 1 if retained_count else 0
    expected_last = begun if retained_count else 0
    if (meta["retained_first_id"] != expected_first or meta["retained_last_id"] != expected_last or
            meta["retained_first_id"] != (rows[0]["id"] if rows else 0) or
            meta["retained_last_id"] != (rows[-1]["id"] if rows else 0)):
        raise ValueError("retained id bounds disagree with ring contents")
    ids = [r["id"] for r in rows]
    if ids != list(range(expected_first, expected_last + 1)):
        raise ValueError("producer-profile retained IDs are missing, duplicated, or out of order")

    complete_count = sum(r["completed"] for r in rows)
    inflight_count = len(rows) - complete_count
    if (complete_count != meta["retained_completed_rows"] or
            inflight_count != meta["retained_inflight_rows"]):
        raise ValueError("retained completed/in-flight counts disagree with rows")
    if complete_count + inflight_count != meta["retained_rows"]:
        raise ValueError("retained lifecycle counts do not partition rows")
    if meta["completed"] + meta["inflight_at_dump"] + meta["late_discarded"] != begun:
        raise ValueError("begun/completed/inflight/late-discarded counts do not balance")
    if meta["late_discarded"] > meta["overwritten_inflight"]:
        raise ValueError("late-discarded count exceeds overwritten in-flight scopes")
    if meta["overwritten_inflight"] > meta["overwritten"]:
        raise ValueError("overwritten-inflight count exceeds overwritten rows")
    if meta["overwritten_inflight"] > meta["late_discarded"] + meta["inflight_at_dump"]:
        raise ValueError("overwritten in-flight scopes are not accounted for at dump")
    if (meta["retained_first_begin_qpc"] !=
            (rows[0]["qpc_begin"] if rows else 0)):
        raise ValueError("retained first-begin QPC disagrees with first ring row")
    if meta["dump_qpc_valid"] and meta["dump_qpc"] == 0:
        raise ValueError("valid dump QPC is zero")

    by_id = {row["id"]: row for row in rows}
    freq = meta["qpc_frequency"]
    for row in rows:
        if row["kind"] not in (0, 1):
            raise ValueError(f"unknown producer-profile kind for id {row['id']}")
        if not row["id"] or row["host_tid"] == 0:
            raise ValueError("invalid producer-profile identity")
        if any(row[key] not in (0, 1) for key in ("completed", "qpc_valid", "cycles_valid")):
            raise ValueError(f"invalid validity/completion flag for id {row['id']}")
        if row["cycles_valid"] and not row["completed"]:
            raise ValueError(f"in-flight scope {row['id']} has valid cycle delta")
        if row["qpc_valid"]:
            if not row["completed"] or row["qpc_begin"] <= 0 or row["qpc_end"] < row["qpc_begin"]:
                raise ValueError(f"invalid QPC interval for scope {row['id']}")
        elif not row["completed"] and row["qpc_end"] != 0:
            raise ValueError(f"in-flight scope {row['id']} has an end QPC")
        if not row["completed"] and (row["qpc_valid"] or row["cycles_valid"] or row["cycles_delta"]):
            raise ValueError(f"in-flight scope {row['id']} has completed timing fields")
        if row["kind"] == 0:
            if row["guest_ea"] != 0x00680FA0:
                raise ValueError(f"invalid root producer scope {row['id']}")
            if row["callsite_lr"] or row["callee_ea"]:
                raise ValueError(f"root producer scope {row['id']} has call-site metadata")
        elif row["guest_ea"] != 0:
            raise ValueError(f"call scope {row['id']} has unexpected guest_ea")
        if row["parent_id"]:
            if row["parent_id"] >= row["id"]:
                raise ValueError(f"scope {row['id']} has a non-ancestral parent ID")
            parent = by_id.get(row["parent_id"])
            # Missing old parents are possible after ring overwrite; retain as
            # an orphan for coverage reporting rather than rejecting the row.
            if parent is None:
                retained_first = expected_first
                if row["parent_id"] >= retained_first:
                    raise ValueError(f"scope {row['id']} has a missing parent inside the retained ID range")
            else:
                if parent["host_tid"] != row["host_tid"]:
                    raise ValueError(f"scope {row['id']} and parent run on different host threads")
                if (parent["qpc_begin"] and row["qpc_begin"] and
                        row["qpc_begin"] < parent["qpc_begin"]):
                    raise ValueError(f"scope {row['id']} begins before its parent")
                if row["qpc_valid"] and parent["qpc_valid"]:
                    if not (parent["qpc_begin"] <= row["qpc_begin"] <= row["qpc_end"] <= parent["qpc_end"]):
                        raise ValueError(f"scope {row['id']} is outside its parent's QPC interval")
        elif row["kind"] == 1:
            raise ValueError(f"call scope {row['id']} is missing a parent ID")
    if sum(r["completed"] for r in rows) > meta["completed"]:
        raise ValueError("retained completions exceed global completion count")
    return by_id, {"retained_completed": complete_count, "retained_inflight": inflight_count,
                   "qpc_invalid_completed": sum(r["completed"] and not r["qpc_valid"] for r in rows),
                   "cycles_invalid_completed": sum(r["completed"] and not r["cycles_valid"] for r in rows)}


def _union_length(intervals: list[tuple[int, int]]) -> int:
    if not intervals:
        return 0
    ordered = sorted((a, b) for a, b in intervals if b > a)
    if not ordered:
        return 0
    total = 0
    start, end = ordered[0]
    for next_start, next_end in ordered[1:]:
        if next_start <= end:
            end = max(end, next_end)
        else:
            total += end - start
            start, end = next_start, next_end
    return total + end - start


def _read_pump(path: Path, frequency: int) -> list[dict]:
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {"qpc_frequency", "frame", "begin_qpc", "end_qpc"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("pump CSV lacks QPC interval fields")
        rows = [{key: _uint(raw[key], key) for key in required} for raw in reader]
    for row in rows:
        if row["qpc_frequency"] != frequency or row["begin_qpc"] <= 0 or row["end_qpc"] <= row["begin_qpc"]:
            raise ValueError("invalid pump QPC frequency or interval")
    if any(a["end_qpc"] > b["begin_qpc"] for a, b in zip(rows, rows[1:])):
        raise ValueError("pump intervals overlap or are out of QPC order")
    return rows


def _percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def analyze(path: Path, qpc_begin: int | None = None, qpc_end: int | None = None,
            pump_path: Path | None = None) -> dict:
    path = Path(path)
    meta, rows = _read_capture(path)
    by_id, validation = _validate(meta, rows)
    if (qpc_begin is None) != (qpc_end is None):
        raise ValueError("both QPC window bounds must be supplied")
    if qpc_begin is not None and (qpc_begin <= 0 or qpc_end <= qpc_begin):
        raise ValueError("invalid QPC window")
    window = (qpc_begin, qpc_end) if qpc_begin is not None else None
    frequency = meta["qpc_frequency"]
    scale = 1000.0 / frequency

    def clip(begin: int, end: int) -> tuple[int, int]:
        if window is None:
            return begin, end
        return max(begin, window[0]), min(end, window[1])

    roots = [r for r in rows if r["kind"] == 0 and r["parent_id"] == 0]
    nested_frames = [r for r in rows if r["kind"] == 0 and r["parent_id"] != 0]
    children_by_parent: dict[int, list[dict]] = {}
    for row in rows:
        if row["parent_id"]:
            children_by_parent.setdefault(row["parent_id"], []).append(row)
    orphans = [r for r in rows if r["parent_id"] and r["parent_id"] not in by_id]

    root_intervals: list[tuple[int, int]] = []
    frame_rows = []
    worst_scopes = []
    child_group: dict[tuple[int, int], dict] = {}
    direct_child_global: list[tuple[int, int]] = []
    root_sum_ticks = 0
    root_cycles = root_cycles_valid = 0
    root_cycles_count = 0
    known_roots = 0
    root_partial = 0
    in_window_rows = 0

    for root in roots:
        if not root["completed"] or not root["qpc_valid"]:
            if window is None or not root["qpc_begin"] or root["qpc_begin"] <= window[1]:
                root_partial += 1
            continue
        lo, hi = clip(root["qpc_begin"], root["qpc_end"])
        if hi <= lo:
            continue
        known_roots += 1
        in_window_rows += 1
        root_ticks = hi - lo
        root_sum_ticks += root_ticks
        root_intervals.append((lo, hi))
        root_ms = root_ticks * scale
        if root["cycles_valid"]:
            root_cycles += root["cycles_delta"]
            root_cycles_valid += 1
            root_cycles_count += 1
        child_spans = []
        for child in children_by_parent.get(root["id"], []):
            if not child["completed"] or not child["qpc_valid"]:
                continue
            c_lo, c_hi = clip(max(lo, child["qpc_begin"]), min(hi, child["qpc_end"]))
            if c_hi <= c_lo:
                continue
            child_spans.append((c_lo, c_hi))
            direct_child_global.append((c_lo, c_hi))
            if child["kind"] != 1:
                continue
            key = (child["callsite_lr"], child["callee_ea"])
            group = child_group.setdefault(key, {
                "callsite_lr": key[0], "callee_ea": key[1], "calls": 0,
                "window_inclusive_ticks": 0, "whole_scope_inclusive_ticks": 0,
                "cycles_valid_calls": 0,
                "cycles_invalid_calls": 0, "cycles_delta_raw_sum": 0,
            })
            group["calls"] += 1
            group["window_inclusive_ticks"] += c_hi - c_lo
            group["whole_scope_inclusive_ticks"] += child["qpc_end"] - child["qpc_begin"]
            if child["cycles_valid"]:
                group["cycles_valid_calls"] += 1
                group["cycles_delta_raw_sum"] += child["cycles_delta"]
            else:
                group["cycles_invalid_calls"] += 1
            c_ms = (c_hi - c_lo) * scale
            worst_scopes.append({"kind": "call", "id": child["id"], "parent_id": root["id"],
                                 "host_tid": child["host_tid"], "callsite_lr": child["callsite_lr"],
                                 "callee_ea": child["callee_ea"], "qpc_begin": child["qpc_begin"],
                                 "qpc_end": child["qpc_end"], "window_ms": c_ms})
        child_union = _union_length(child_spans)
        frame_rows.append({"id": root["id"], "host_tid": root["host_tid"],
                           "qpc_begin": root["qpc_begin"], "qpc_end": root["qpc_end"],
                           "window_ms": root_ms,
                           "direct_child_union_ms": child_union * scale,
                           "uncovered_by_profiled_direct_calls_ms": max(0, root_ticks-child_union) * scale,
                           "cycles_valid": bool(root["cycles_valid"]),
                           "cycles_delta_raw": root["cycles_delta"] if root["cycles_valid"] else None})
        worst_scopes.append({"kind": "frame", "id": root["id"], "parent_id": 0,
                             "host_tid": root["host_tid"], "qpc_begin": root["qpc_begin"],
                             "qpc_end": root["qpc_end"], "window_ms": root_ms})

    root_union = _union_length(root_intervals)
    child_union_global = _union_length(direct_child_global)
    call_groups = []
    for group in child_group.values():
        group["window_inclusive_wall_ms"] = group.pop("window_inclusive_ticks") * scale
        group["whole_scope_inclusive_wall_ms"] = group.pop("whole_scope_inclusive_ticks") * scale
        call_groups.append(group)
    call_groups.sort(key=lambda g: g["window_inclusive_wall_ms"], reverse=True)
    worst_scopes.sort(key=lambda s: s["window_ms"], reverse=True)

    # Include nested dispatcher calls without folding their inclusive time
    # into root totals. Parent function identity separates identical targets
    # reached from different enclosing phases.
    nested_groups = {}
    for row in rows:
        if row["kind"] != 1 or not row["completed"] or not row["qpc_valid"]:
            continue
        lo, hi = clip(row["qpc_begin"], row["qpc_end"])
        if hi <= lo:
            continue
        parent = by_id.get(row["parent_id"])
        parent_ea = (parent["guest_ea"] if parent["kind"] == 0 else parent["callee_ea"]) if parent else None
        key = (parent_ea, row["callsite_lr"], row["callee_ea"])
        group = nested_groups.setdefault(key, {
            "parent_function_ea": parent_ea, "callsite_lr": row["callsite_lr"],
            "callee_ea": row["callee_ea"], "calls": 0,
            "window_inclusive_wall_ms": 0.0, "whole_scope_wall_ms": [],
            "cycles_delta_raw_whole_scope_sum": 0, "cycles_valid_calls": 0,
        })
        group["calls"] += 1
        group["window_inclusive_wall_ms"] += (hi-lo) * scale
        group["whole_scope_wall_ms"].append((row["qpc_end"]-row["qpc_begin"]) * scale)
        if row["cycles_valid"]:
            group["cycles_valid_calls"] += 1
            group["cycles_delta_raw_whole_scope_sum"] += row["cycles_delta"]
    for group in nested_groups.values():
        values = group.pop("whole_scope_wall_ms")
        group["whole_scope_distribution_ms"] = {
            "mean": sum(values)/len(values), "p95": _percentile(values, .95),
            "p99": _percentile(values, .99), "max": max(values),
        }
    all_call_groups = sorted(nested_groups.values(), key=lambda g: g["window_inclusive_wall_ms"], reverse=True)

    pump_report = None
    if pump_path is not None:
        pumps = _read_pump(Path(pump_path), frequency)
        ends = [r["end_qpc"] for r in pumps]
        by_pump: dict[int, list[tuple[int, int]]] = {}
        for start, end in root_intervals:
            index = bisect.bisect_right(ends, start)
            while index < len(pumps) and pumps[index]["begin_qpc"] < end:
                pump = pumps[index]
                overlap = (max(start, pump["begin_qpc"]), min(end, pump["end_qpc"]))
                if overlap[1] > overlap[0]:
                    by_pump.setdefault(index, []).append(overlap)
                index += 1
        pump_rows = []
        pump_union_ticks = 0
        for index, intervals in by_pump.items():
            union = _union_length(intervals)
            pump_union_ticks += union
            pump_rows.append({"pump_frame": pumps[index]["frame"],
                              "root_intervals": len(intervals),
                              "root_qpc_union_ms": union * scale})
        pump_rows.sort(key=lambda r: r["root_qpc_union_ms"], reverse=True)
        pump_report = {"matched_pump_intervals": len(by_pump),
                       "root_qpc_union_ms": pump_union_ticks * scale,
                       "worst_pump_intervals": pump_rows[:20],
                       "join": "QPC overlap only; pump frame IDs are labels, never join keys."}

    reasons = []
    if window is not None:
        w0, w1 = window
        if meta["dump_qpc_valid"] and meta["dump_qpc"] < w1:
            reasons.append("requested window extends beyond dump QPC")
        elif not meta["dump_qpc_valid"]:
            reasons.append("dump QPC is unavailable")
        if meta["max_discarded_end_qpc"] > w0:
            reasons.append("a discarded completed scope may overlap the requested window")
        elif meta["overwritten"] and not meta["max_discarded_end_qpc"]:
            reasons.append("overwritten scopes have no usable end-QPC coverage")
        if meta["overwritten_inflight"]:
            earliest = meta["earliest_overwritten_inflight_begin_qpc"]
            if not earliest or earliest <= w1:
                reasons.append("an overwritten in-flight scope may overlap the requested window")
        if meta["discarded_invalid_qpc_observations"]:
            reasons.append("discarded scopes include invalid QPC observations")
        for row in rows:
            if not row["completed"] and (not row["qpc_begin"] or row["qpc_begin"] <= w1):
                reasons.append("an unfinished retained scope may overlap the requested window")
                break
    elif meta["overwritten"] or meta["inflight_at_dump"] or meta["late_discarded"]:
        reasons.append("capture contains overwritten, late-discarded, or unfinished scopes")
    if any(not r["qpc_valid"] for r in rows if r["completed"]):
        reasons.append("some completed scopes have invalid QPC timing")
    coverage_complete = not reasons and not meta["overwritten"] and not meta["inflight_at_dump"]

    return {
        "source": str(path), "qpc_frequency": frequency,
        "window": ({"begin_qpc": window[0], "end_qpc": window[1],
                    "duration_ms": (window[1]-window[0]) * scale} if window else None),
        "capture": {**meta, **validation,
                    "complete_for_capture": not meta["overwritten"] and not meta["inflight_at_dump"] and not meta["late_discarded"],
                    "requested_window_coverage_proven": False,
                    "coverage_note": "CSV has no capture-start QPC; a requested window cannot be proven to lie inside the recording lifetime."},
        "window_coverage": {"potentially_incomplete": bool(reasons), "reasons": sorted(set(reasons)),
                            "known_completed_valid_qpc_only": True},
        "roots": {"completed_valid_qpc_frames": known_roots,
                  "nested_frame_scopes_excluded_from_root_totals": len(nested_frames),
                  "unfinished_or_invalid_frames_potentially_in_window": root_partial,
                  "inclusive_sum_ms": root_sum_ticks * scale,
                  "interval_union_ms_no_nested_double_count": root_union * scale,
                  "direct_child_union_ms_no_nested_double_count": child_union_global * scale,
                  "uncovered_by_profiled_direct_calls_union_ms": max(0, root_union-child_union_global) * scale,
                  "cycles_delta_raw_sum": root_cycles,
                  "cycles_valid_frames": root_cycles_valid,
                  "cycles_invalid_or_unavailable_frames": max(0, known_roots-root_cycles_count),
                  "per_frame_distribution_ms": {"count": len(frame_rows),
                     "mean": (sum(r["window_ms"] for r in frame_rows)/len(frame_rows) if frame_rows else None),
                     "p50": _percentile([r["window_ms"] for r in frame_rows], .50),
                     "p95": _percentile([r["window_ms"] for r in frame_rows], .95),
                     "p99": _percentile([r["window_ms"] for r in frame_rows], .99),
                     "max": max((r["window_ms"] for r in frame_rows), default=None)},
                  "per_frame": sorted(frame_rows, key=lambda r: r["window_ms"], reverse=True)[:50]},
        "direct_calls_grouped_by_lr_callee": call_groups,
        "all_calls_grouped_by_parent_lr_callee": all_call_groups,
        "all_calls_note": "Inclusive nested groups overlap: never sum them as frame cost. Cycle deltas and distribution samples cover whole scopes even at clipped window boundaries.",
        "worst_scopes": worst_scopes[:50],
        "orphan_calls": {"count": len(orphans),
                         "ids": [r["id"] for r in orphans[:50]],
                         "note": "A missing parent below the retained ID range is consistent with ring overwrite; other missing parents are schema errors."},
        "pump_qpc_overlap": pump_report,
        "interpretation": "Scope and call wall times are inclusive. Raw cycle deltas are not CPU time. Excluded timer-helper calls and non-call work remain in the direct-call-uncovered remainder.",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--qpc-begin", type=int)
    parser.add_argument("--qpc-end", type=int)
    parser.add_argument("--pump", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.trace, args.qpc_begin, args.qpc_end, args.pump)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
