#!/usr/bin/env python3
"""Compile actual shader-bundle codec/persistence helpers and exercise them."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
BEGIN = "/* BEGIN RSX SHADER BUNDLE */"
END = "/* END RSX SHADER BUNDLE */"
PERSIST_BEGIN = "/* BEGIN RSX SHADER BUNDLE PERSIST */"
PERSIST_END = "/* END RSX SHADER BUNDLE PERSIST */"


def extract_block(source: str, begin: str, end_marker: str) -> str:
    start = source.find(begin)
    end = source.find(end_marker, start + len(begin))
    if start < 0 or end < 0:
        raise ValueError("shader bundle source markers are missing")
    return source[start + len(begin):end]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define MAX_SHADER_BLOBS 8192u
#define SHADER_DISK_CACHE_VERSION 2u
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)
'''


HARNESS = r'''
#include <thread>
#include <vector>

static shader_bundle_item make_item(u32 stage, const u8* source, u32 source_length,
                                    const u8* blob, u32 blob_length) {
    shader_bundle_item item = {};
    item.stage = stage;
    item.hash = shader_bundle_hash(source, source_length);
    item.source_length = source_length;
    item.blob_length = blob_length;
    item.source = source;
    item.blob = blob;
    return item;
}

static void must_reject(const u8* original, size_t size, size_t offset,
                        const void* replacement, size_t replacement_size) {
    assert(offset + replacement_size <= size);
    std::vector<u8> bytes(original, original + size);
    memcpy(bytes.data() + offset, replacement, replacement_size);
    shader_bundle_view view = {};
    assert(!shader_bundle_parse(bytes.data(), bytes.size(), &view));
    assert(!view.bytes && !view.count && !view.records);
}

static int persist_one(const char* path, u8 value) {
    u8 source[] = {'w','o','r','k','e','r','-', value};
    u8 blob[] = {'b','l','o','b','-', value};
    shader_bundle_item item = {};
    item.stage = (u32)'V';
    item.hash = shader_bundle_hash(source, sizeof(source));
    item.source_length = sizeof(source);
    item.blob_length = sizeof(blob);
    item.source = source;
    item.blob = blob;
    u32 saved_entries = 0;
    size_t saved_bytes = 0;
    return shader_bundle_persist_file(path, &item, 1,
                                      &saved_entries, &saved_bytes);
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--write")) {
        if (argc != 4 || strlen(argv[3]) != 1) return 2;
        return persist_one(argv[2], (u8)argv[3][0]) ? 0 : 3;
    }
    if (argc >= 4 && !strcmp(argv[1], "--wait-write")) {
        if (argc != 6 || strlen(argv[3]) != 1) return 2;
        HANDLE ready = CreateFileA(argv[5], GENERIC_WRITE, 0, NULL,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ready == INVALID_HANDLE_VALUE) return 8;
        CloseHandle(ready);
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (GetFileAttributesA(argv[4]) == INVALID_FILE_ATTRIBUTES) {
            if (GetTickCount64() >= deadline) return 4;
            Sleep(1);
        }
        return persist_one(argv[2], (u8)argv[3][0]) ? 0 : 5;
    }
    if (argc == 3 && !strcmp(argv[1], "--validate")) {
        u8* bytes = NULL;
        size_t size = 0;
        shader_bundle_view view = {};
        if (shader_bundle_read_file(argv[2], &bytes, &size, &view) != 1)
            return 6;
        for (u8 value : {'S', 'B', 'C'}) {
            u8 source[] = {'w','o','r','k','e','r','-', value};
            if (!shader_bundle_find(&view, (u32)'V', (const char*)source,
                    sizeof(source), shader_bundle_hash(source, sizeof(source)))) {
                free(bytes);
                return 7;
            }
        }
        free(bytes);
        return 0;
    }
    if (argc != 1) return 2;
    static const u8 src_v[] = {'v','s',' ','m','a','i','n'};
    static const u8 src_p[] = {'p','s',' ','m','a','i','n'};
    static const u8 src_new[] = {'p','s',' ','n','e','w'};
    static const u8 blob_v[] = {1, 3, 5, 7};
    static const u8 blob_p[] = {2, 4, 6};
    static const u8 blob_new[] = {8, 9};
    shader_bundle_item initial[2] = {
        make_item((u32)'V', src_v, sizeof(src_v), blob_v, sizeof(blob_v)),
        make_item((u32)'P', src_p, sizeof(src_p), blob_p, sizeof(blob_p))
    };
    u8* archive = NULL;
    size_t archive_size = 0;
    assert(shader_bundle_serialize(initial, 2, &archive, &archive_size));
    assert(archive && archive_size <= SHADER_BUNDLE_MAX_BYTES);
    shader_bundle_view view = {};
    assert(shader_bundle_parse(archive, archive_size, &view));
    assert(view.count == 2 && view.size == archive_size);
    assert(view.records[0].stage == (u32)'P' && view.records[1].stage == (u32)'V');

    const shader_bundle_record* record = shader_bundle_find(
        &view, (u32)'V', (const char*)src_v, sizeof(src_v),
        shader_bundle_hash(src_v, sizeof(src_v)));
    assert(record && record->blob_length == sizeof(blob_v));
    assert(!memcmp(view.bytes + record->blob_offset, blob_v, sizeof(blob_v)));
    static const u8 same_length_different_source[] = {'x','s',' ','m','a','i','n'};
    assert(!shader_bundle_find(&view, (u32)'V',
        (const char*)same_length_different_source,
        sizeof(same_length_different_source), record->hash));
    assert(!shader_bundle_find(&view, (u32)'P', (const char*)src_v,
        sizeof(src_v), record->hash));

    /* Exact duplicate records collapse; a duplicate key with changed blob is
     * rejected instead of silently selecting one payload. */
    shader_bundle_item duplicates[2] = {
        make_item((u32)'V', src_v, sizeof(src_v), blob_v, sizeof(blob_v)),
        make_item((u32)'V', src_v, sizeof(src_v), blob_v, sizeof(blob_v))
    };
    u8* duplicate_archive = NULL;
    size_t duplicate_size = 0;
    assert(shader_bundle_serialize(duplicates, 2, &duplicate_archive, &duplicate_size));
    shader_bundle_view duplicate_view = {};
    assert(shader_bundle_parse(duplicate_archive, duplicate_size, &duplicate_view));
    assert(duplicate_view.count == 1);
    static const u8 changed_blob[] = {0, 0, 0, 0};
    shader_bundle_item ambiguous[2] = {
        make_item((u32)'V', src_v, sizeof(src_v), blob_v, sizeof(blob_v)),
        make_item((u32)'V', src_v, sizeof(src_v), changed_blob, sizeof(changed_blob))
    };
    u8* rejected = NULL; size_t rejected_size = 0;
    assert(!shader_bundle_serialize(ambiguous, 2, &rejected, &rejected_size));
    assert(!rejected && rejected_size == 0);

    /* Merge a prior archive with one new shader and an exact repeat. */
    shader_bundle_item merged[4] = {};
    for (u32 i = 0; i < view.count; ++i) {
        const shader_bundle_record* old = &view.records[i];
        merged[i].stage = old->stage;
        merged[i].hash = old->hash;
        merged[i].source_length = old->source_length;
        merged[i].blob_length = old->blob_length;
        merged[i].source = view.bytes + old->source_offset;
        merged[i].blob = view.bytes + old->blob_offset;
    }
    merged[2] = make_item((u32)'P', src_new, sizeof(src_new), blob_new, sizeof(blob_new));
    merged[3] = make_item((u32)'V', src_v, sizeof(src_v), blob_v, sizeof(blob_v));
    u8* merged_archive = NULL; size_t merged_size = 0;
    assert(shader_bundle_serialize(merged, 4, &merged_archive, &merged_size));
    shader_bundle_view merged_view = {};
    assert(shader_bundle_parse(merged_archive, merged_size, &merged_view));
    assert(merged_view.count == 3);
    assert(shader_bundle_find(&merged_view, (u32)'P', (const char*)src_p,
        sizeof(src_p), shader_bundle_hash(src_p, sizeof(src_p))));
    assert(shader_bundle_find(&merged_view, (u32)'P', (const char*)src_new,
        sizeof(src_new), shader_bundle_hash(src_new, sizeof(src_new))));
    assert(shader_bundle_find(&merged_view, (u32)'V', (const char*)src_v,
        sizeof(src_v), shader_bundle_hash(src_v, sizeof(src_v))));

    /* Header/version/bounds and record corruption must fail closed. */
    shader_bundle_view unused = {};
    assert(!shader_bundle_parse(archive, archive_size - 1u, &unused));
    assert(!shader_bundle_parse(NULL, archive_size, &unused));
    assert(!shader_bundle_parse(archive, archive_size, NULL));
    const size_t h = sizeof(shader_bundle_header);
    must_reject(archive, archive_size, offsetof(shader_bundle_header, magic),
                "BADMAGIC", 8);
    u32 value;
#define REJECT_HEADER(field, replacement_value) do { \
    value = (replacement_value); \
    must_reject(archive, archive_size, offsetof(shader_bundle_header, field), \
                &value, sizeof(value)); \
} while (0)
    REJECT_HEADER(schema_version, SHADER_BUNDLE_SCHEMA_VERSION + 1u);
    REJECT_HEADER(disk_cache_version, SHADER_DISK_CACHE_VERSION + 1u);
    REJECT_HEADER(compiler_tag, SHADER_BUNDLE_COMPILER_TAG ^ 1u);
    REJECT_HEADER(entry_count, SHADER_BUNDLE_MAX_ENTRIES + 1u);
    REJECT_HEADER(entry_size, sizeof(shader_bundle_record) + 1u);
    REJECT_HEADER(total_size, (u32)archive_size - 1u);
    REJECT_HEADER(index_offset, (u32)sizeof(shader_bundle_header) + 1u);
    REJECT_HEADER(data_offset, (u32)h);
    const size_t rec = sizeof(shader_bundle_header);
#define REJECT_RECORD(field, replacement_value) do { \
    value = (replacement_value); \
    must_reject(archive, archive_size, rec + offsetof(shader_bundle_record, field), \
                &value, sizeof(value)); \
} while (0)
    REJECT_RECORD(stage, (u32)'X');
    REJECT_RECORD(reserved, 1u);
    REJECT_RECORD(source_length, SHADER_BUNDLE_MAX_SOURCE + 1u);
    REJECT_RECORD(blob_length, 0u);
    REJECT_RECORD(blob_length, SHADER_BUNDLE_MAX_BLOB + 1u);
    REJECT_RECORD(source_offset, 0xfffffff0u);
    REJECT_RECORD(blob_offset, 0xfffffff0u);
    {
        const u64 bad_hash = view.records[0].hash ^ 1u;
        must_reject(archive, archive_size,
                    rec + offsetof(shader_bundle_record, hash),
                    &bad_hash, sizeof(bad_hash));
    }
    {
        const u8 bad_payload = (u8)(view.bytes[view.records[0].source_offset] ^ 1u);
        must_reject(archive, archive_size, view.records[0].source_offset,
                    &bad_payload, sizeof(bad_payload));
    }
    {
        const u8 bad_payload = (u8)(view.bytes[view.records[0].blob_offset] ^ 1u);
        must_reject(archive, archive_size, view.records[0].blob_offset,
                    &bad_payload, sizeof(bad_payload));
    }
    {
        const shader_bundle_record first = view.records[0];
        must_reject(archive, archive_size, rec + sizeof(shader_bundle_record),
                    &first, sizeof(first));
    }

    /* Count and total-byte caps reject before payload allocation/copy. */
    static const u8 one_byte = 7;
    const u32 over_entry_cap = SHADER_BUNDLE_MAX_ENTRIES + 1u;
    shader_bundle_item* repeated = (shader_bundle_item*)calloc(
        over_entry_cap, sizeof(shader_bundle_item));
    assert(repeated);
    for (u32 i = 0; i < over_entry_cap; ++i)
        repeated[i] = make_item((u32)'V', src_v, sizeof(src_v),
                                blob_v, sizeof(blob_v));
    u8* repeated_archive = NULL; size_t repeated_size = 0;
    assert(shader_bundle_serialize(repeated, over_entry_cap,
                                   &repeated_archive, &repeated_size));
    shader_bundle_view repeated_view = {};
    assert(shader_bundle_parse(repeated_archive, repeated_size, &repeated_view));
    assert(repeated_view.count == 1);
    free(repeated);
    free(repeated_archive);

    shader_bundle_item* too_many = (shader_bundle_item*)calloc(
        over_entry_cap, sizeof(shader_bundle_item));
    u8 (*unique_sources)[4] = (u8 (*)[4])calloc(over_entry_cap, 4);
    assert(too_many && unique_sources);
    for (u32 i = 0; i < over_entry_cap; ++i) {
        memcpy(unique_sources[i], &i, 4);
        too_many[i] = make_item((u32)'V', unique_sources[i], 4,
                                &one_byte, 1);
    }
    assert(!shader_bundle_serialize(too_many, SHADER_BUNDLE_MAX_ENTRIES + 1u,
                                    &rejected, &rejected_size));
    free(too_many);
    free(unique_sources);
    const u32 large_count = SHADER_BUNDLE_MAX_BYTES / SHADER_DISK_CACHE_MAX_BLOB + 1u;
    shader_bundle_item* oversized = (shader_bundle_item*)calloc(large_count, sizeof(shader_bundle_item));
    assert(oversized);
    u8 sources[32][4];
    for (u32 i = 0; i < large_count; ++i) {
        memcpy(sources[i], &i, 4);
        oversized[i] = make_item((u32)'V', sources[i], 4,
                                 &one_byte, SHADER_DISK_CACHE_MAX_BLOB);
    }
    assert(!shader_bundle_serialize(oversized, large_count, &rejected, &rejected_size));
    free(oversized);

    /* Concurrent parse/find on one immutable archive and serialize on
     * independent lists exercises the helper's shared-read/reentrant paths. */
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&view, thread]() {
            for (unsigned i = 0; i < 1000; ++i) {
                const shader_bundle_record* found = shader_bundle_find(
                    &view, (u32)'V', (const char*)src_v, sizeof(src_v),
                    shader_bundle_hash(src_v, sizeof(src_v)));
                assert(found);
                shader_bundle_item local = make_item(
                    (u32)'P', src_p, sizeof(src_p), blob_p, sizeof(blob_p));
                u8* bytes = NULL; size_t size = 0;
                assert(shader_bundle_serialize(&local, 1, &bytes, &size));
                shader_bundle_view parsed = {};
                assert(shader_bundle_parse(bytes, size, &parsed));
                assert(parsed.count == 1 && thread < 8);
                free(bytes);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    free(archive);
    free(duplicate_archive);
    free(merged_archive);
    return 0;
}
'''


def run_persistence_race(executable: Path, directory: Path) -> None:
    archive = directory / "concurrent.bundle"
    gate = directory / "release.gate"
    ready_b = directory / "ready-b"
    ready_c = directory / "ready-c"
    # Seed an entry first, then release two independent writers together. Each
    # process rereads the current archive under the sidecar lock before merge.
    subprocess.run([str(executable), "--write", str(archive), "S"], check=True)
    first = subprocess.Popen([str(executable), "--wait-write", str(archive), "B",
                              str(gate), str(ready_b)])
    second = subprocess.Popen([str(executable), "--wait-write", str(archive), "C",
                               str(gate), str(ready_c)])
    deadline = time.monotonic() + 10
    while not (ready_b.exists() and ready_c.exists()):
        if time.monotonic() >= deadline:
            first.kill()
            second.kill()
            raise TimeoutError("persistence workers did not reach the gate")
        time.sleep(0.005)
    gate.write_text("go", encoding="ascii")
    assert first.wait(timeout=20) == 0
    assert second.wait(timeout=20) == 0
    subprocess.run([str(executable), "--validate", str(archive)], check=True)

    # Existing malformed data must be preserved and must not be overwritten
    # by a persistence attempt.
    corrupt = directory / "corrupt.bundle"
    original = b"not a shader bundle\x00with trailing bytes"
    corrupt.write_bytes(original)
    result = subprocess.run([str(executable), "--write", str(corrupt), "X"])
    assert result.returncode != 0
    assert corrupt.read_bytes() == original


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, help="clang-cl executable")
    parser.add_argument("--source", type=Path,
                        default=ROOT / "external/ps3recomp/libs/video/rsx_live_draw.c")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    source_text = args.source.read_text(encoding="utf-8")
    block = extract_block(source_text, BEGIN, END)
    persist_block = extract_block(source_text, PERSIST_BEGIN, PERSIST_END)
    if args.output_dir:
        out = args.output_dir.resolve()
        out.mkdir(parents=True, exist_ok=True)
        context = tempfile.TemporaryDirectory(dir=out)
    else:
        context = tempfile.TemporaryDirectory()
    with context as temp:
        temp_path = Path(temp)
        fixture = temp_path / "shader_bundle_fixture.cpp"
        executable = temp_path / "shader_bundle_fixture.exe"
        fixture.write_text(PRELUDE + block + persist_block + HARNESS, encoding="utf-8")
        command = [args.compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                   "/O2", str(fixture), f"/Fo:{temp_path / 'shader_bundle_fixture.obj'}",
                   f"/Fe:{executable}"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
        run_persistence_race(executable, temp_path)
    print("PASS shader bundle codec/persistence: corruption, merge, caps, concurrent writers")


if __name__ == "__main__":
    main()
