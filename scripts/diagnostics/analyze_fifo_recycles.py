"""Validate and summarize retained producer-side FIFO recycle events.

Joins to pump intervals and optional QPC windows use timestamps only; renderer
frame labels in the recycle CSV are descriptive and are not join keys. Ring
events are bounded and callbacks can still be in flight at shutdown.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path


CAPACITY = 16384
OUTCOMES = {0: "complete", 1: "null_context", 2: "empty", 3: "unmapped"}
FIELDS = {
    "sequence", "host_tid", "renderer_frame", "qpc_begin", "qpc_end",
    "total_ms", "context_ea", "begin_ea", "current_ea", "begin_io",
    "current_io", "outcome", "ack_current_expected_ea",
    "ack_current_initial_observed_ea", "ack_current_observed_ea",
    "ack_current_begin", "ack_current_end", "ack_current_ms",
    "ack_current_wait_calls", "ack_current_completed",
    "ack_begin_expected_ea", "ack_begin_initial_observed_ea",
    "ack_begin_observed_ea", "ack_begin_begin", "ack_begin_end",
    "ack_begin_ms", "ack_begin_wait_calls", "ack_begin_completed",
}
ACKS = ("ack_current", "ack_begin")


def _parse_int(value: str, field: str) -> int:
    try:
        return int(value, 16) if value.lower().startswith("0x") else int(value, 10)
    except (AttributeError, ValueError) as exc:
        raise ValueError(f"invalid integer in {field}: {value!r}") from exc


def _parse_metadata(line: str) -> dict[str, int]:
    if not line.startswith("#"):
        raise ValueError("missing recycle metadata row")
    values = next(csv.reader([line[1:].strip()]))
    if len(values) % 2 or len(values[::2]) != len(set(values[::2])):
        raise ValueError("malformed or duplicate recycle metadata keys")
    metadata = {_key: _parse_int(_value, _key)
                for _key, _value in zip(values[::2], values[1::2])}
    required = {"qpc_frequency", "recorded", "overwritten", "started", "completed", "inflight_at_dump"}
    if not required.issubset(metadata):
        raise ValueError(f"missing metadata fields: {sorted(required - metadata.keys())}")
    if metadata["qpc_frequency"] <= 0 or any(metadata[k] < 0 for k in required - {"qpc_frequency"}):
        raise ValueError("invalid negative or zero recycle metadata")
    return metadata


def _read_capture(path: Path) -> tuple[dict[str, int], list[dict]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        metadata = _parse_metadata(stream.readline())
        reader = csv.DictReader(stream)
        if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise ValueError("missing or duplicate recycle CSV header")
        missing = FIELDS - set(reader.fieldnames)
        if missing:
            raise ValueError(f"missing recycle columns: {sorted(missing)}")
        rows = []
        for line_number, raw in enumerate(reader, start=3):
            if None in raw or any(value is None for value in raw.values()):
                raise ValueError(f"nonrectangular recycle row at line {line_number}")
            row = {}
            for key, value in raw.items():
                row[key] = (float(value) if key.endswith("_ms") else _parse_int(value, key))
                if isinstance(row[key], float) and not math.isfinite(row[key]):
                    raise ValueError(f"nonfinite value in {key} at line {line_number}")
            rows.append(row)
    return metadata, rows


def _validate(metadata: dict[str, int], rows: list[dict]) -> dict:
    recorded, overwritten = metadata["recorded"], metadata["overwritten"]
    retained = min(recorded, CAPACITY)
    if overwritten != max(0, recorded - CAPACITY) or len(rows) != retained:
        raise ValueError("recorded/overwritten counts disagree with ring capacity or retained rows")
    started, completed, inflight = (metadata[k] for k in ("started", "completed", "inflight_at_dump"))
    if started != completed + inflight:
        raise ValueError("started callbacks do not equal completed plus in-flight at dump")
    if completed != recorded:
        raise ValueError("completed callbacks do not equal recorded callback count")

    sequences = [row["sequence"] for row in rows]
    expected = list(range(overwritten + 1, recorded + 1))
    if sequences != expected:
        raise ValueError("retained sequence rows are missing, duplicated, or out of order")

    frequency = metadata["qpc_frequency"]
    scale = 1000.0 / frequency
    tolerance = 0.000002
    outcomes = {code: 0 for code in OUTCOMES}
    for row in rows:
        if row["host_tid"] <= 0 or row["renderer_frame"] < 0:
            raise ValueError("invalid event identity")
        if row["outcome"] not in OUTCOMES:
            raise ValueError("unknown recycle outcome")
        outcomes[row["outcome"]] += 1
        start, end = row["qpc_begin"], row["qpc_end"]
        if start <= 0 or end < start:
            raise ValueError("invalid event QPC interval")
        if row["total_ms"] < 0 or abs(row["total_ms"] - (end - start) * scale) > tolerance:
            raise ValueError("event milliseconds disagree with QPC")

        ack_intervals = []
        for ack in ACKS:
            prefix = ack + "_"
            ack_start, ack_end = row[prefix + "begin"], row[prefix + "end"]
            complete = row[prefix + "completed"]
            if complete not in (0, 1) or row[prefix + "wait_calls"] < 0:
                raise ValueError(f"invalid {ack} completion or wait count")
            if complete:
                if not start <= ack_start <= ack_end <= end:
                    raise ValueError(f"{ack} interval is outside its callback")
                expected_ea = row[prefix + "expected_ea"]
                initial_ea = row[prefix + "initial_observed_ea"]
                observed_ea = row[prefix + "observed_ea"]
                if observed_ea != expected_ea:
                    raise ValueError(f"{ack} did not observe its expected EA")
                if row[prefix + "ms"] < 0 or abs(row[prefix + "ms"] - (ack_end - ack_start) * scale) > tolerance:
                    raise ValueError(f"{ack} milliseconds disagree with QPC")
                if initial_ea == expected_ea and row[prefix + "wait_calls"] != 0:
                    raise ValueError(f"{ack} waited despite an initially satisfied EA")
                if initial_ea != expected_ea and row[prefix + "wait_calls"] == 0:
                    raise ValueError(f"{ack} reports no wait for an initially unsatisfied EA")
                ack_intervals.append((ack_start, ack_end))
            else:
                if any(row[prefix + field] != 0 for field in (
                    "begin", "end", "ms", "wait_calls", "expected_ea",
                    "initial_observed_ea", "observed_ea")):
                    raise ValueError(f"uncompleted {ack} contains partial timing data")
        if row["outcome"] == 0:
            if len(ack_intervals) != 2 or ack_intervals[0][1] > ack_intervals[1][0]:
                raise ValueError("completed callback must contain ordered current-then-begin acknowledgements")
            if (row["ack_current_expected_ea"] != row["current_ea"] or
                    row["ack_begin_expected_ea"] != row["begin_ea"]):
                raise ValueError("ack expected EAs disagree with the callback current/begin EAs")
        elif ack_intervals:
            raise ValueError("early-return callback unexpectedly contains an acknowledgement")
    return {"retained": retained, "outcomes": outcomes, "scale_ms_per_qpc": scale}


def _read_pump(path: Path, frequency: int) -> list[dict]:
    rows = []
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {"qpc_frequency", "frame", "begin_qpc", "end_qpc"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("pump CSV lacks QPC interval columns")
        for number, raw in enumerate(reader, start=2):
            row = {key: _parse_int(raw[key], key) for key in required}
            if row["qpc_frequency"] != frequency:
                raise ValueError("pump and recycle QPC frequencies differ")
            if row["begin_qpc"] <= 0 or row["end_qpc"] <= row["begin_qpc"]:
                raise ValueError(f"invalid pump interval at line {number}")
            rows.append(row)
    if any(a["end_qpc"] > b["begin_qpc"] for a, b in zip(rows, rows[1:])):
        raise ValueError("pump intervals overlap or are out of QPC order")
    return rows


def analyze(path: Path, qpc_begin: int | None = None, qpc_end: int | None = None,
            pump_path: Path | None = None) -> dict:
    path = Path(path)
    metadata, events = _read_capture(path)
    validation = _validate(metadata, events)
    if (qpc_begin is None) != (qpc_end is None):
        raise ValueError("both QPC window bounds must be supplied")
    if qpc_begin is not None and (qpc_begin <= 0 or qpc_end <= qpc_begin):
        raise ValueError("invalid QPC window")
    frequency, scale = metadata["qpc_frequency"], validation["scale_ms_per_qpc"]
    pumps = _read_pump(Path(pump_path), frequency) if pump_path else []
    pump_starts = [pump["begin_qpc"] for pump in pumps]
    pump_ends = [pump["end_qpc"] for pump in pumps]

    callback_ms = current_ms = begin_ms = 0.0
    current_calls = begin_calls = current_wait_events = begin_wait_events = 0
    selected_events = []
    pump_overlaps = []
    for event in events:
        lo = event["qpc_begin"] if qpc_begin is None else max(event["qpc_begin"], qpc_begin)
        hi = event["qpc_end"] if qpc_end is None else min(event["qpc_end"], qpc_end)
        clipped = max(0, hi - lo)
        current_overlap = begin_overlap = 0
        for ack, sink in (("ack_current", "current"), ("ack_begin", "begin")):
            if event[ack + "_completed"]:
                a = max(event[ack + "_begin"], qpc_begin) if qpc_begin is not None else event[ack + "_begin"]
                b = min(event[ack + "_end"], qpc_end) if qpc_end is not None else event[ack + "_end"]
                if b > a:
                    if sink == "current": current_overlap = b - a
                    else: begin_overlap = b - a
        callback_ms += clipped * scale
        current_ms += current_overlap * scale
        begin_ms += begin_overlap * scale
        if clipped:
            # These are whole-callback wait counts for events intersecting the
            # selected QPC interval; the individual calls are not timestamped.
            current_calls += event["ack_current_wait_calls"]
            begin_calls += event["ack_begin_wait_calls"]
            current_wait_events += int(event["ack_current_wait_calls"] > 0)
            begin_wait_events += int(event["ack_begin_wait_calls"] > 0)
            selected_events.append(event)
        overlapping = []
        pump_index = bisect.bisect_right(pump_ends, lo)
        while pump_index < len(pumps) and pump_starts[pump_index] < hi:
            pump = pumps[pump_index]
            overlap = max(0, min(hi, pump["end_qpc"]) - max(lo, pump["begin_qpc"]))
            if overlap:
                item = {"pump_frame": pump["frame"], "overlap_qpc": overlap,
                        "overlap_ms": overlap * scale}
                overlapping.append(item)
                pump_overlaps.append({"sequence": event["sequence"], **item})
            pump_index += 1
        event["window_overlap_qpc"] = clipped
        event["window_overlap_ms"] = clipped * scale
        event["pump_overlaps"] = overlapping

    worst = sorted(selected_events, key=lambda row: row["window_overlap_qpc"], reverse=True)[:10]
    complete_coverage = metadata["overwritten"] == 0 and metadata["inflight_at_dump"] == 0
    if qpc_begin is None:
        window = None
    else:
        window = {"begin_qpc": qpc_begin, "end_qpc": qpc_end,
                  "duration_ms": (qpc_end - qpc_begin) * scale}
    return {
        "source": str(path), "qpc_frequency": frequency, "window": window,
        "coverage": {"recorded": metadata["recorded"], "retained": validation["retained"],
                     "overwritten": metadata["overwritten"], "started": metadata["started"],
                     "completed": metadata["completed"], "inflight_at_dump": metadata["inflight_at_dump"],
                     "complete_for_capture": complete_coverage,
                     "note": ("complete_for_capture means only that all completed callbacks at dump were retained; "
                              "overwritten events or callbacks still active make it false. The CSV has no capture "
                              "start/end timestamps, so this does not establish that a requested QPC window was "
                              "inside the recording lifetime.")},
        "outcomes": {OUTCOMES[key]: value for key, value in validation["outcomes"].items()},
        "summary": {"selected_callbacks": len(selected_events), "callback_ms": callback_ms,
                    "current_ack_ms": current_ms, "begin_ack_ms": begin_ms,
                    "current_wait_calls_whole_intersecting_callbacks": current_calls,
                    "begin_wait_calls_whole_intersecting_callbacks": begin_calls,
                    "current_callbacks_with_waits": current_wait_events,
                    "begin_callbacks_with_waits": begin_wait_events,
                    "worst_intervals": [{"sequence": e["sequence"], "host_tid": e["host_tid"],
                                         "renderer_frame": e["renderer_frame"],
                                         "qpc_begin": e["qpc_begin"], "qpc_end": e["qpc_end"],
                                         "window_overlap_ms": e["window_overlap_ms"],
                                         "outcome": OUTCOMES[e["outcome"]]} for e in worst]},
        "pump_overlap_by_qpc": pump_overlaps,
        "events": events,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--qpc-begin", type=int)
    parser.add_argument("--qpc-end", type=int)
    parser.add_argument("--pump", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.trace, args.qpc_begin, args.qpc_end, args.pump)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
