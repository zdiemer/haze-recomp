# Native performance checkpoint

This checkpoint brings two bounded changes from the performance branch onto
current main. It references [#38](https://github.com/zdiemer/haze-recomp/issues/38)
and [#59](https://github.com/zdiemer/haze-recomp/issues/59), with continued work
tracked under [#9](https://github.com/zdiemer/haze-recomp/issues/9) and
[#32](https://github.com/zdiemer/haze-recomp/issues/32).

## Changes

Ordinary native byte and halfword guest reads use the existing uninstrumented
paths already available to browser builds. Bounds checks, low-address checks
and enabled memory probes retain their original behavior. Unaligned halfword
reads preserve guest big-endian values. Stores and synchronization are unchanged.

Supported 2D block-compressed texture uploads can reuse an immutable GPU resource
when every mip's bytes and layout match, even when guest addresses differ.
Hashes select candidates; exact bytes, format, dimensions, row layout and mip
count authorize a hit. Uploads use the same snapshot used for lookup. A changed
texture gets its own resource. Each returned binding and retained entry own
separate references, so eviction cannot invalidate an existing binding.

The cache retains at most 256 entries, 64 MiB of CPU snapshots including scratch,
and 64 MiB of GPU allocation. Temporary allocator overhead and resources kept
alive by active bindings are outside those retention limits. Cubemaps, vertex
textures and ordinary RGBA uploads keep their existing paths. Entire mip chains
are preflighted before resource creation: insufficient upload space returns a
failure without caching a partially uploaded texture.

No additional graphics setting or performance toggle is introduced. This
checkpoint preserves main's audio, controls and graphics policies. The broader
GPU geometry, native worker, job-kernel and completion-wait experiments remain
on the performance branch.

## Evidence and limits

The memory-read fixture extracts the actual SDK accessors and compares their
values and diagnostic routes with the previous native implementation. It covers
unaligned and boundary reads, out-of-bounds and low addresses, forced values,
each diagnostic selector, and a read-sampling build. Unrelated diagnostic
implementations are stubbed; this verifies accessor routing, not every probe.

The D3D12 WARP texture tests exercise exact-content relocation, changed bytes,
format/mip/layout differences, hash collisions, clear and eviction reference
lifetimes, upload exhaustion and retry, and GPU readback of uploaded contents.

Earlier full-experiment captures saw 12,074 of 12,077 eligible texture requests
reuse storage across four windows. Those captures contained other changes and
overlapping Haze instances: they establish the eliminated allocations in those
windows, not an isolated FPS gain for this checkpoint. The recent 89.9/94.7 FPS
jungle results likewise belong to the broader experimental binary and must not
be advertised as results for this smaller checkpoint.

Consistent frame pacing, loading behavior, broad campaign coverage and 144 FPS
remain open. The pre-existing local jump-table discovery defect is tracked
separately in [#78](https://github.com/zdiemer/haze-recomp/issues/78).
