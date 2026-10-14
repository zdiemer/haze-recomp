#!/usr/bin/env python3
"""Summarize bounded RSX texture-miss traces and reconcile measured windows."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


TRACE_FIELDS = (
    "event", "frame", "qpc", "use_serial", "slot", "evicted",
    "previous_use_serial", "location", "offset", "format", "width",
    "height", "pitch", "remap", "cubemap", "mipmaps", "dimension",
    "content_hash", "decoded",
)
DESCRIPTOR_FIELDS = (
    "location", "offset", "format", "width", "height", "pitch",
    "remap", "cubemap", "mipmaps", "dimension",
)
CONTENT_GROUP_FIELDS = tuple(
    field for field in DESCRIPTOR_FIELDS if field not in ("location", "offset")
) + ("content_hash",)
RENDERER_MISS_FIELD = "texture_decode_miss_calls"
U64_MASK = (1 << 64) - 1
U32_MASK = (1 << 32) - 1


class TraceError(ValueError):
    """Input CSV or window metadata is incomplete or malformed."""


def _integer(value: str | None, field: str, row_number: int) -> int:
    if value is None or value == "":
        raise TraceError(f"row {row_number}: missing {field}")
    try:
        return int(value, 10)
    except ValueError as exc:
        raise TraceError(f"row {row_number}: invalid decimal {field}: {value!r}") from exc


def read_trace(path: Path) -> list[dict[str, Any]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise TraceError(f"{path}: empty CSV")
        missing = [name for name in TRACE_FIELDS if name not in reader.fieldnames]
        if missing:
            raise TraceError(f"{path}: missing columns: {', '.join(missing)}")
        rows: list[dict[str, Any]] = []
        for row_number, raw in enumerate(reader, 2):
            if None in raw:
                raise TraceError(f"{path}: row {row_number}: extra/truncated CSV fields")
            row = dict(raw)
            for name in TRACE_FIELDS:
                if row.get(name) is None or row[name] == "":
                    raise TraceError(f"{path}: row {row_number}: missing {name}")
            for name in TRACE_FIELDS:
                if name == "decoded":
                    value = _integer(row[name], name, row_number)
                    if value not in (0, 1):
                        raise TraceError(f"{path}: row {row_number}: decoded must be 0 or 1")
                    row[name] = bool(value)
                else:
                    value = _integer(row[name], name, row_number)
                    if value < 0:
                        raise TraceError(f"{path}: row {row_number}: {name} must be unsigned")
                    row[name] = value
            for name in ("event", "use_serial", "previous_use_serial", "content_hash"):
                if row[name] > U64_MASK:
                    raise TraceError(f"{path}: row {row_number}: {name} exceeds uint64")
            if row["frame"] > U32_MASK:
                raise TraceError(f"{path}: row {row_number}: frame exceeds uint32")
            if row["qpc"] > (1 << 63) - 1:
                raise TraceError(f"{path}: row {row_number}: qpc exceeds signed QPC range")
            if row["evicted"] not in (0, 1):
                raise TraceError(f"{path}: row {row_number}: evicted must be 0 or 1")
            rows.append(row)
    if not rows:
        raise TraceError(f"{path}: no trace rows")
    return rows


def read_windows(path: Path) -> list[dict[str, int]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TraceError(f"{path}: cannot read windows JSON: {exc}") from exc
    blocks = payload.get("blocks") if isinstance(payload, dict) else None
    if not isinstance(blocks, list) or not blocks:
        raise TraceError(f"{path}: expected a non-empty blocks array")
    result = []
    for index, block in enumerate(blocks):
        try:
            start, end = int(block["start_qpc"]), int(block["end_qpc"])
        except (KeyError, TypeError, ValueError) as exc:
            raise TraceError(f"{path}: block {index}: requires integer start_qpc/end_qpc") from exc
        if start > end:
            raise TraceError(f"{path}: block {index}: start_qpc exceeds end_qpc")
        result.append({"index": index, "start_qpc": start, "end_qpc": end})
    return result


def read_renderer(path: Path) -> list[dict[str, int]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        if len(fields) != 119:
            raise TraceError(f"{path}: expected 119 renderer columns, found {len(fields)}")
        if len(fields) != len(set(fields)):
            raise TraceError(f"{path}: duplicate renderer column names")
        if "qpc" not in fields or "frame" not in fields or RENDERER_MISS_FIELD not in fields:
            raise TraceError(f"{path}: requires frame, qpc, and {RENDERER_MISS_FIELD} columns")
        rows = []
        for row_number, raw in enumerate(reader, 2):
            if None in raw or any(raw.get(field) in (None, "") for field in fields):
                raise TraceError(f"{path}: row {row_number}: truncated/missing renderer field")
            numeric = {}
            for field in fields:
                try:
                    value = float(raw[field])
                except ValueError as exc:
                    raise TraceError(f"{path}: row {row_number}: invalid numeric {field}: {raw[field]!r}") from exc
                if not math.isfinite(value):
                    raise TraceError(f"{path}: row {row_number}: non-finite {field}")
                if value < 0 and not (field == "gpu_ms" and value == -1):
                    raise TraceError(f"{path}: row {row_number}: {field} must be unsigned")
                numeric[field] = value
            try:
                frame = int(raw["frame"], 10)
                qpc = int(raw["qpc"], 10)
                miss_calls = int(raw[RENDERER_MISS_FIELD], 10)
            except ValueError as exc:
                raise TraceError(f"{path}: row {row_number}: frame, qpc, and miss calls must be integers") from exc
            if not (0 <= frame <= U32_MASK):
                raise TraceError(f"{path}: row {row_number}: frame exceeds uint32")
            if not (0 <= qpc <= (1 << 63) - 1):
                raise TraceError(f"{path}: row {row_number}: qpc outside signed QPC range")
            if miss_calls < 0:
                raise TraceError(f"{path}: row {row_number}: miss calls must be unsigned")
            rows.append({"frame": frame, "qpc": qpc, RENDERER_MISS_FIELD: miss_calls})
    return rows


def _continuity(rows: list[dict[str, Any]], field: str) -> dict[str, Any]:
    gaps = []
    for previous, current in zip(rows, rows[1:]):
        expected = (previous[field] + 1) & U64_MASK
        if current[field] != expected:
            gaps.append({"after": previous[field], "before": current[field], "expected": expected})
    return {"ok": not gaps, "gap_count": len(gaps), "examples": gaps[:10]}


def _monotonic(rows: list[dict[str, Any]], field: str) -> dict[str, Any]:
    decreases = []
    for index, (previous, current) in enumerate(zip(rows, rows[1:]), 1):
        # QPC and serial can wrap as unsigned 64-bit values.
        if current[field] < previous[field] and not (
            previous[field] == U64_MASK and current[field] == 0
        ):
            decreases.append({"row_pair": index, "previous": previous[field], "current": current[field]})
    return {"ok": not decreases, "decrease_count": len(decreases), "examples": decreases[:10]}


def _tuple_key(row: dict[str, Any], fields: Iterable[str]) -> tuple[int, ...]:
    return tuple(int(row[field]) for field in fields)


def _repeated_misses(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], int]:
    grouped: dict[tuple[int, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[_tuple_key(row, DESCRIPTOR_FIELDS)].append(row)
    repeated = []
    for key, events in grouped.items():
        if len(events) < 2:
            continue
        repeated.append({
            "descriptor": dict(zip(DESCRIPTOR_FIELDS, key)),
            "misses": len(events),
            "decoded": sum(bool(row["decoded"]) for row in events),
            "evictions": sum(bool(row["evicted"]) for row in events),
            "first_event": events[0]["event"],
            "last_event": events[-1]["event"],
            "first_qpc": events[0]["qpc"],
            "last_qpc": events[-1]["qpc"],
        })
    repeated.sort(key=lambda item: (-item["misses"], item["first_event"]))
    return repeated, len(grouped)


def _identity_sensitivity(rows: list[dict[str, Any]], unique_full: int) -> dict[str, int]:
    result = {"full_descriptor": unique_full}
    for ignored in DESCRIPTOR_FIELDS:
        fields = tuple(field for field in DESCRIPTOR_FIELDS if field != ignored)
        result[f"ignoring_{ignored}"] = len({_tuple_key(row, fields) for row in rows})
    return result


def _source_variation(rows: list[dict[str, Any]], limit: int = 10) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[(row["location"], row["offset"])].append(row)
    result = []
    for (location, offset), events in grouped.items():
        varying = []
        for field in DESCRIPTOR_FIELDS[2:]:
            count = len({row[field] for row in events})
            if count > 1:
                varying.append({"field": field, "distinct_values": count})
        varying.sort(key=lambda value: (-value["distinct_values"], value["field"]))
        if varying:
            result.append({
                "location": location,
                "offset": offset,
                "misses": len(events),
                "distinct_descriptors": len({_tuple_key(row, DESCRIPTOR_FIELDS) for row in events}),
                "varying_fields": varying,
            })
    result.sort(key=lambda item: (-len(item["varying_fields"]), -item["misses"], item["location"], item["offset"]))
    return result[:limit]


def _layout_content_groups(rows: list[dict[str, Any]], limit: int = 10) -> dict[str, Any]:
    grouped: dict[tuple[int, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[_tuple_key(row, CONTENT_GROUP_FIELDS)].append(row)
    repeated = [(key, events) for key, events in grouped.items() if len(events) > 1]
    multi_source = [
        (key, events) for key, events in grouped.items()
        if len({(row["location"], row["offset"]) for row in events}) > 1
    ]
    repeated.sort(key=lambda item: (-len(item[1]), item[0]))
    return {
        "grouping_fields": list(CONTENT_GROUP_FIELDS),
        "group_count": len(grouped),
        "repeated_group_count": len(repeated),
        "misses_in_repeated_groups": sum(len(events) for _, events in repeated),
        "multi_source_group_count": len(multi_source),
        "top_repeated_groups": [
            {
                "key": dict(zip(CONTENT_GROUP_FIELDS, key)),
                "misses": len(events),
                "distinct_sources": len({(row["location"], row["offset"]) for row in events}),
                "decoded": sum(bool(row["decoded"]) for row in events),
            }
            for key, events in repeated[:limit]
        ],
        "caveat": (
            "Grouping omits source location/offset and uses content_hash plus descriptor layout. "
            "This identifies candidate relocation clusters; hash equality is not an exact-byte comparison."
        ),
    }


def _window_summary(
    rows: list[dict[str, Any]], windows: list[dict[str, int]],
    renderer: list[dict[str, int]] | None,
) -> dict[str, Any]:
    selected_counts = Counter()
    trace_misses_by_frame = Counter(row["frame"] for row in rows)
    summaries = []
    for window in windows:
        start, end = window["start_qpc"], window["end_qpc"]
        selected = [row for row in rows if start <= row["qpc"] <= end]
        descriptor_counts = Counter(_tuple_key(row, DESCRIPTOR_FIELDS) for row in selected)
        selected_counts.update(row["event"] for row in selected)
        item: dict[str, Any] = {
            **window,
            "miss_rows": len(selected),
            "event_time_miss_rows": len(selected),
            "decoded": sum(bool(row["decoded"]) for row in selected),
            "evictions": sum(bool(row["evicted"]) for row in selected),
            "unique_descriptors": len(descriptor_counts),
            "repeated_descriptor_keys": sum(count > 1 for count in descriptor_counts.values()),
            "layout_content_groups": _layout_content_groups(selected),
            "coverage": "matched" if selected else "no_trace_rows",
        }
        if renderer is not None:
            renderer_rows = [row for row in renderer if start <= row["qpc"] <= end]
            renderer_total = sum(row[RENDERER_MISS_FIELD] for row in renderer_rows)
            # texture_profile_miss records g_ld_frames before the upcoming
            # rsx_live_draw_present increments it. ld_profile_present then
            # writes that incremented value, so ordinary presents map trace
            # frame N to renderer frame N+1 (with uint32 wrap).
            frame_mismatch_examples = []
            matched_frames = 0
            frame_matched_miss_rows = 0
            for renderer_row in renderer_rows:
                source_frame = (renderer_row["frame"] - 1) & U32_MASK
                trace_frame_count = trace_misses_by_frame[source_frame]
                matches = trace_frame_count == renderer_row[RENDERER_MISS_FIELD]
                matched_frames += matches
                frame_matched_miss_rows += trace_frame_count
                if not matches and len(frame_mismatch_examples) < 10:
                    frame_mismatch_examples.append({
                        "renderer_frame": renderer_row["frame"],
                        "trace_frame": source_frame,
                        "trace_miss_rows": trace_frame_count,
                        "renderer_miss_calls": renderer_row[RENDERER_MISS_FIELD],
                    })
            reconciles = bool(renderer_rows) and matched_frames == len(renderer_rows)
            event_frames = sorted({row["frame"] for row in selected})
            renderer_frames = [row["frame"] for row in renderer_rows]
            item.update({
                "renderer_rows": len(renderer_rows),
                "renderer_miss_calls": renderer_total,
                "renderer_frames": {
                    "count": len(renderer_frames),
                    "first": renderer_frames[0] if renderer_frames else None,
                    "last": renderer_frames[-1] if renderer_frames else None,
                },
                "event_time_trace_frames": {
                    "count": len(event_frames),
                    "first": event_frames[0] if event_frames else None,
                    "last": event_frames[-1] if event_frames else None,
                },
                "frame_matched_trace_miss_rows": frame_matched_miss_rows,
                "frame_matched_rows": matched_frames,
                "frame_mismatch_count": len(renderer_rows) - matched_frames,
                "frame_mismatch_examples": frame_mismatch_examples,
                "renderer_reconciles": reconciles,
            })
            if not renderer_rows:
                item["coverage"] = "no_renderer_rows"
        summaries.append(item)
    covered_events = {event for event, count in selected_counts.items() if count}
    overlaps = sum(count > 1 for count in selected_counts.values())
    return {
        "blocks": summaries,
        "trace_rows_covered": len(covered_events),
        "trace_rows_outside_windows": len(rows) - len(covered_events),
        "trace_rows_in_overlapping_windows": overlaps,
        "empty_trace_blocks": sum(block["miss_rows"] == 0 for block in summaries),
        "blocks_with_trace_rows": sum(block["miss_rows"] > 0 for block in summaries),
        "renderer_reconciliation_ok": (
            all(block.get("renderer_reconciles", False) for block in summaries)
            if renderer is not None else None
        ),
    }


def analyze(
    trace_rows: list[dict[str, Any]], windows: list[dict[str, int]] | None = None,
    renderer: list[dict[str, int]] | None = None,
) -> dict[str, Any]:
    repeated, unique_count = _repeated_misses(trace_rows)
    hashes = Counter(row["content_hash"] for row in trace_rows)
    descriptors_by_hash: dict[int, set[tuple[int, ...]]] = defaultdict(set)
    for row in trace_rows:
        descriptors_by_hash[row["content_hash"]].add(_tuple_key(row, DESCRIPTOR_FIELDS))
    event_check = _continuity(trace_rows, "event")
    qpc_check = _monotonic(trace_rows, "qpc")
    serial_check = _monotonic(trace_rows, "use_serial")
    validation_ok = event_check["ok"] and qpc_check["ok"] and serial_check["ok"]
    result: dict[str, Any] = {
        "frame_mapping": {
            "ordinary_live_present": "texture miss frame N maps to renderer frame N+1",
            "basis": "miss logs g_ld_frames before present; renderer profile logs after increment",
            "caveat": "movie-overlay guest flips can increment g_ld_frames without a renderer-present row",
        },
        "rows": len(trace_rows),
        "event_range": [trace_rows[0]["event"], trace_rows[-1]["event"]],
        "qpc_range": [trace_rows[0]["qpc"], trace_rows[-1]["qpc"]],
        "use_serial_range": [trace_rows[0]["use_serial"], trace_rows[-1]["use_serial"]],
        "decoded_rows": sum(bool(row["decoded"]) for row in trace_rows),
        "eviction_rows": sum(bool(row["evicted"]) for row in trace_rows),
        "unique_full_descriptors": unique_count,
        "repeated_descriptor_keys": len(repeated),
        "repeated_miss_keys": repeated,
        "unique_counts_ignoring_one_field": _identity_sensitivity(trace_rows, unique_count),
        "source_offset_variation_top": _source_variation(trace_rows),
        "layout_content_groups": _layout_content_groups(trace_rows),
        "content_hashes": {
            "unique_count": len(hashes),
            "repeated_hashes": sum(count > 1 for count in hashes.values()),
            "max_hash_repetitions": max(hashes.values()),
            "hashes_spanning_multiple_descriptors": sum(
                len(descriptors) > 1 for descriptors in descriptors_by_hash.values()
            ),
            "interpretation": (
                "Content-hash equality is only a byte-content clue. Different formats, dimensions, "
                "remaps, mip layouts, or view types can make equal hashes distinct GPU resources."
            ),
        },
        "validation": {
            "ok": validation_ok,
            "event_continuity": event_check,
            "qpc_monotonic": qpc_check,
            "use_serial_monotonic": serial_check,
        },
    }
    if windows is not None:
        result["windows"] = _window_summary(trace_rows, windows, renderer)
        if renderer is not None:
            result["validation"]["renderer_window_reconciliation"] = result["windows"]["renderer_reconciliation_ok"]
            result["validation"]["ok"] = (
                result["validation"]["ok"] and result["windows"]["renderer_reconciliation_ok"]
            )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="*.texture-misses.csv trace")
    parser.add_argument("--windows", type=Path, help="JSON file containing blocks with start_qpc/end_qpc")
    parser.add_argument("--renderer119colCSV", type=Path, help="119-column renderer CSV to reconcile per-window miss counts")
    parser.add_argument("--output", type=Path, required=True, help="write full JSON analysis here")
    args = parser.parse_args(argv)
    try:
        rows = read_trace(args.trace)
        windows = read_windows(args.windows) if args.windows else None
        if args.renderer119colCSV and windows is None:
            raise TraceError("--renderer119colCSV requires --windows")
        renderer = read_renderer(args.renderer119colCSV) if args.renderer119colCSV else None
        result = analyze(rows, windows, renderer)
        result["inputs"] = {
            "trace": str(args.trace),
            "windows": str(args.windows) if args.windows else None,
            "renderer119colCSV": str(args.renderer119colCSV) if args.renderer119colCSV else None,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (OSError, TraceError) as exc:
        parser.error(str(exc))
    windows_text = ""
    if "windows" in result:
        windows_text = f", windows={len(result['windows']['blocks'])}, covered={result['windows']['trace_rows_covered']}"
        if result["windows"]["renderer_reconciliation_ok"] is not None:
            windows_text += f", renderer_match={result['windows']['renderer_reconciliation_ok']}"
    print(
        f"rows={result['rows']}, unique_descriptors={result['unique_full_descriptors']}, "
        f"repeated_keys={result['repeated_descriptor_keys']}, "
        f"event/qpc/serial_ok={result['validation']['event_continuity']['ok']}/"
        f"{result['validation']['qpc_monotonic']['ok']}/"
        f"{result['validation']['use_serial_monotonic']['ok']}{windows_text}"
    )
    return 0 if result["validation"]["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
