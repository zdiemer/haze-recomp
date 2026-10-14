"""Read local Haze PBCK archives for native menu resource inspection.

Extracted resources belong in ignored local storage, never in the repository.
The reader does not change the disc or its installed archives.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import zlib


@dataclass(frozen=True)
class Entry:
    index: int
    key: int
    size: int
    compressed_size: int
    offset: int
    metadata: int


class Pak:
    def __init__(self, path: Path):
        self.path = Path(path)
        data = self.path.read_bytes()
        if len(data) < 24:
            raise ValueError("Truncated PBCK header")
        magic, table, count, auxiliary, auxiliary_size, version = struct.unpack_from(">6I", data)
        self.shards = version >> 16
        if magic != 0x5042434B or table != 24 or (version & 65535) != 2 or not 1 <= self.shards <= 256:
            raise ValueError("Unsupported PBCK header")
        if auxiliary != table + count * 20 or auxiliary + auxiliary_size != len(data):
            raise ValueError("Invalid PBCK table bounds")
        self.entries = []
        for index in range(count):
            key, size, compressed, offset, packed = struct.unpack_from(">5I", data, table + index * 20)
            if (packed & 255) >= self.shards:
                raise ValueError("PBCK entry references a missing shard")
            self.entries.append(Entry(index, key, size, compressed, offset, packed))

    def read(self, entry: Entry, limit: int = 16 * 1024 * 1024) -> bytes:
        if entry.size > limit:
            raise ValueError(f"Entry {entry.index} exceeds extraction limit")
        # The low metadata byte selects the data shard. The remaining metadata
        # and mixed raw/compressed blocks are not decoded by this tool.
        shard = Path(f"{self.path}.{entry.metadata & 255:02d}")
        length = entry.compressed_size or entry.size
        with shard.open("rb") as stream:
            stream.seek(0, 2)
            if entry.offset + length > stream.tell():
                raise ValueError(f"Entry {entry.index} extends beyond shard")
            stream.seek(entry.offset)
            data = stream.read(length)
        if not entry.compressed_size:
            return data
        # Files larger than 16 KiB contain concatenated independent zlib
        # streams. A single decompress() silently returns only the first one.
        result = bytearray()
        while data:
            decoder = zlib.decompressobj()
            try:
                block = decoder.decompress(data, entry.size - len(result) + 1)
            except zlib.error as error:
                raise ValueError(f"Entry {entry.index}: invalid zlib stream") from error
            result.extend(block)
            if len(result) > entry.size or not decoder.eof:
                raise ValueError(f"Entry {entry.index}: truncated or oversized zlib stream")
            data = decoder.unused_data
        if len(result) != entry.size:
            raise ValueError(f"Entry {entry.index}: decoded size mismatch")
        return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pak", type=Path)
    parser.add_argument("--contains", action="append", default=[], help="Match UTF-8 text anywhere in decoded resources")
    parser.add_argument("--entry", action="append", type=int, default=[])
    parser.add_argument("--output", type=Path, required=True, help="Ignored local extraction directory")
    parser.add_argument("--max-size", type=int, default=2 * 1024 * 1024)
    args = parser.parse_args()
    if not args.contains and not args.entry:
        parser.error("Supply --contains or --entry")
    archive = Pak(args.pak)
    if any(index < 0 or index >= len(archive.entries) for index in args.entry):
        parser.error("Entry index outside archive")
    needles = [value.encode() for value in args.contains]
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    skipped = []
    for entry in archive.entries:
        explicit = entry.index in args.entry
        if not explicit and (entry.size > args.max_size or not needles):
            continue
        try:
            data = archive.read(entry, args.max_size)
        except ValueError as error:
            if explicit:
                raise
            skipped.append({"entry": entry.index, "reason": str(error)})
            continue
        if not explicit and not any(needle in data for needle in needles):
            continue
        suffix = ".res" if data.startswith(b"//") else ".bin"
        name = f"{args.pak.stem}_{entry.index:05d}_{entry.key:08x}{suffix}"
        (args.output / name).write_bytes(data)
        report.append({"file": name, "entry": entry.index, "key": f"{entry.key:08x}",
                       "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    (args.output / f"{args.pak.stem}-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.output / f"{args.pak.stem}-skipped.json").write_text(json.dumps(skipped, indent=2) + "\n")
    print(f"Extracted {len(report)} matching resources to {args.output}")
    if skipped:
        print(f"{len(skipped)} entries could not be decoded; see {args.pak.stem}-skipped.json")


if __name__ == "__main__":
    main()
