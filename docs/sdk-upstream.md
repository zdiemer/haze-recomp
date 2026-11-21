# SDK integration and upstream review

## November 2026 review

Reviewed on 2026-11-21: upstream `0fe7dbf..d81c393e` (v0.20.0, 617
non-merge commits) against the fork's `haze` at `9eed2aa6`, which was also the
pin. `git cherry` found no patch-identical commits already in the fork.

Imported with `git cherry-pick -x` (zdiemer/ps3recomp#14):

| Upstream | Reason |
| --- | --- |
| `b074d964` | Event queues were marked active before their critical section was initialised, so a host producer could enter a zeroed lock. New race test: crashes 3/3 before, passes after. |
| `daa17fbc` | stwcx./stdcx. re-check the reservation after taking the lock-line lock. |
| `20122d59` | Reservation contexts were added on every thread start; after about 127 starts new contexts were never scanned. The test was adapted to `ppu_resv_break_store`, because the fork has no `ppu_resv_break_line`. |
| `b3a75d98`, `48958fef` | Every HLE→guest callback started at `0xCFFE0000`, so concurrent or nested callbacks shared frames. Each host thread now gets its own callback stack (top 16 MB of the stack region). |
| `cd926aa1`, `86353eb3` | Per-thread HLE NID and indirect-call-target caches; the GCM pump skips its lock when idle. |
| `f0b28621` | An empty XInput slot cost about 140 µs per poll; empty slots are now re-polled once a second. |

Measured on the frame-locked later-combat benchmark ([benchmark
clock](benchmark-clock.md)), 5 ABBA pairs: −0.19 ms (−1.2%) draw-adjusted,
p≈0.28. That is within noise, so these are integrated as correctness fixes,
not as a speedup.

Deferred:

| Upstream | Reason |
| --- | --- |
| `6bea2d59`, `81344af1`, `4609b3f0` | SPU channel wake/event-status atomicity and Win64 halt longjmp. They touch `spu_channels.c`, which the port hash-guards; re-audit the port extensions first. |
| `ff7c9eef`, `e2589800` | Atomic taskset bit updates; they intersect Haze's SPURS task ownership in `spu_workload.c` (also guarded). |
| `fcfc3054`, `fb7da2bf` and related | SPURS per-worker wakes need upstream's persistent coordinator scheduler (the deferred `dbff6e0` lineage). |
| `dc37f00b`, `9a3fbffd`, `61978373`, `f453a07f` | lwmutex atomics and handoff. Measure whether Haze is lwmutex-bound first; handoff can create convoys. |
| `9efa74be`, `9d6f012a` | Host-boot time base and real-clock flip timestamps; the latter must route through Haze's benchmark clock. |
| `2e028084`, `3b27abdb`, `62dce119`, `07f1aed3` | Adapter selection (renderer is hash-guarded), walker getenv caching, directory-open retries and free-space overflow: low risk, left for a later pass. |
| Fragment-program semantics, FIFO walker rework, cellAudio, SDL2 pad, Vulkan/Metal, lifter-only changes | Would invalidate validated shader bundles, overlap Haze's own implementations, or matter only for a re-lift. |

## October 2026 review

Reviewed on 2026-10-16 from Haze Recomp `origin/main` at `99d5e72`.
The SDK pin was `2f67212fb16214854d0662aba7161b5c786726e6`, while the fork's
published `haze` was `4b69e56b70c41255684ec6bc89d04b9959debca0`.

### Restore the integration branch

The pin was five commits ahead of `haze`, with no divergent commits on `haze`:

| Commit | Shipped change |
| --- | --- |
| `9008d15` | Exact-validated fragment-program metadata reuse |
| `96e5ec7` | Prepare known graphics pipelines before gameplay |
| `3408d97` | Preserve sampler descriptors across bounded heap pages |
| `b6c2a72` | Larger bounded texture-content reuse |
| `2f67212` | Correct sampler-lifetime texture fixture initialization |

The integration preserves these exact commits and adds the selected fixes
below. Published `haze` now includes integration tip
[`cc62dea`](https://github.com/zdiemer/ps3recomp/commit/cc62dea), which is the
new parent pin. All 79 distinct SDK pins found in `origin/main` history are ancestors
of this integration. Unshipped experimental branches are not automatically
merged into the supported runtime.

`.gitmodules` already selected `haze`; the missing protection was checking
whether the gitlink belonged to that branch. `scripts/check_sdk_pin.py --fetch`
now checks the committed canonical URL/branch, checkout identity, cleanliness
and ancestry against the fetched fork branch. Asset-free CI and the release
workflow run it. See `AGENTS.md` for the publication order.

### Upstream comparison

Upstream is [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp).
The reviewed tip is
[`0fe7dbf5db9a467aca268865fb26170ea113c541`](https://github.com/sp00nznet/ps3recomp/commit/0fe7dbf5db9a467aca268865fb26170ea113c541)
(v0.15.0 integration). The common ancestor with the fork's `haze` is `5244db9`.
Before this integration, `haze...upstream/master` contained 171 fork-only and
266 upstream-only commits; 160 upstream-only commits were non-merges. Commit
counts overstate missing fixes because Haze already has equivalent work under
different SHAs.

A non-checkout `git merge-tree --write-tree --name-only 2f67212 upstream/master`
preview reports conflicts in **29 files**, including SPURS scheduling, SPU
arithmetic/coherence, PPU loading, rendering, SAIL and lifters. A full sync is
possible, but needs a separate integration and gameplay validation effort.
Cherry-picking isolated fixes keeps this update reviewable.

### Selected upstream fixes

| Upstream commit | Reason and validation |
| --- | --- |
| [`7e257f7`](https://github.com/sp00nznet/ps3recomp/commit/7e257f757c7fa1791908a1c8589c8126cf7facce) | SPU decrementer elapsed-time multiplication overflowed after about 231 seconds. Split seconds/remainder before scaling. Add a compiled regression executing the actual channel body against a Python arbitrary-precision oracle around overflow, legitimate counter wraps, fractional ticks, startup and long sessions. |
| [`87016a0`](https://github.com/sp00nznet/ps3recomp/commit/87016a0702c8294a3388ed9be49487160f1c7c7e) | A lifted function ending at `sc` must continue at the next instruction. Preserve upstream's synthetic instruction/lifter regression. This fixes future lifts; existing generated game code is not regenerated by this update. |

Both are imported with `git cherry-pick -x` and their tests are wired into
`scripts/build.ps1 -Test`, including the asset-free runtime path. Neither
change is evidence of a measured gameplay performance improvement.

Validation: Windows `build.ps1 -SharedRoot <prepared-checkout> -RuntimeOnly
-Test` passed. The decrementer test fails against `2f67212` at
231,162,206,438 elapsed nanoseconds and passes with the fix. The upstream
fall-through checks pass. Pin-hygiene tests use isolated real Git histories,
including a local bare fork for the fetch path. Full game relifting and
gameplay were not performed for this update.

### Already covered or deferred

| Upstream area | Decision |
| --- | --- |
| Sampler cache identity (`7f2da09`) | Already covered: Haze's 64-bit key includes all wrap/LOD fields plus shadow and quality-override state. Keep the more complete implementation and heap-page lifetime fixes. |
| Windows large files (`8eb45f8`) | Already covered by Haze's `ps3_host_*` 64-bit seek/stat wrappers, including descriptor-based fstat. |
| Predicated fragment writes (`e99cbbf`) | Already covered by `8169ce5`; Haze emits component selects without nonfinite arithmetic. |
| FNMS extended float (`6903c28`) and broad SPU arithmetic (`b8cb0c8`) | Haze has exact extended-range arithmetic and a tested native path. Reconcile against its arithmetic/audio tests before replacing helpers. |
| SPU signal wake, taskset guest fields, ILH and RI10 decoding from v0.14 | Already integrated through Haze-specific fixes and regression fixtures. |
| New SPURS scheduling/context registry and task requeue fixes (`dbff6e0`, `89cc4c5`, `c4ae58d`) | Promising follow-up, but intersects Haze's task ownership, parking and worker scheduling. Needs focused lifetime tests and sustained gameplay. |
| Broader lifter/VMX changes (`12158c4`, `65806f3`, `a470b79`) | Review with synthetic instruction tests and a separately regenerated lift; current game patches deliberately guard known generated source bodies. |
| New save-data PSF serialization (`927bf4c`) | Requires compatibility tests with existing Haze profiles, saves and co-op state. |
| Vulkan/Metal rendering, gamma/depth and neutral engine changes | Much is outside the native D3D12 path; reconcile render targets, shadow behavior and cache identity with Haze captures before adopting. |

For the next review, fetch both repositories and compare from the exact
published pin. Check behavior and tests as well as commit ancestry; do not
replace Haze fixes just to reduce the ahead/behind count.
