# Haze Recomp

Experimental native Windows port of the PS3 version of **Haze**, using
[ps3recomp](https://github.com/sp00nznet/ps3recomp). This is a development
project, not a playable release.

## Target and requested features

The initial target is the USA disc, **BLUS30094**, disc version **01.02**,
application version **01.00**. Other regions and updates are not validated.

| Feature | Status |
| --- | --- |
| Standalone native Windows executable | Renders menus and the opening campaign scene, including squad, weapons, carrier and sky |
| Campaign gameplay, audio, saves | Fresh opening cinematic, first checkpoint/save and first squad-gathering objective verified; movement/camera and pause/resume work; later combat, mission completion and sound quality unvalidated |
| Online peer-to-peer campaign co-op | Not implemented or validated for Haze |
| Full keyboard and mouse controls | Configurable WASD and all pad buttons; raw mouse directly rotates the player camera in degrees per device count |
| PC key prompts | Planned; requires UI/asset work |
| Higher frame rates | Duplicate presentations fixed; native simulation and frame pacing optimization in progress |
| Custom rendering resolution | Live D3D12 renderer accepts explicit scene/output dimensions; native sizes remain the default |
| Ultrawide | Research after baseline rendering works; projection and HUD must both be addressed |

The runtime building successfully does not establish that Haze works. Haze's
network imports include the older `sceNp` API and ticket requests; upstream
`sceNpMatching2` support alone does not supply Haze matchmaking. Direct peer
traffic may still need session discovery/signaling, NAT traversal, or a VPN.
The game's actual session protocol must be investigated before choosing one.

The native frame pump now consumes each flip notification when presenting it.
The previous count-based fallback left the notification armed: a diagnostic
sample had 1,536 presents for 992 guest flips, including empty second batches.
The corrected carrier run reports zero duplicate presents. The FPS display now
uses a recent 1–2 second window instead of retaining menu rates for 15 seconds.
This fixes misleading telemetry and duplicate presentation; low simulation
rates and remaining rendering artifacts still need work. Set
`HAZE_FRAME_TRACE=1` for flip/present counts. `scripts/build.ps1 -ProfileRenderer`
enables optional renderer timing counters; normal builds omit that overhead.

The live renderer now handles Haze's `D8R8G8B8` texture format, preserves the
128-byte spacing between complete cubemap mip pyramids, and includes mip count
and dimension in texture-cache identity. The cache holds 4,096 descriptors and
uses hashed lookups instead of scanning every entry at each binding. Asset-free
WARP tests read back all six cube faces and opaque RGB texels through the actual
uploader, and exercise cache collisions, replacement and mip-count differences.
These repair specific rendering errors; they do not establish that all visible
flicker, shadows or other graphical artifacts are resolved.

Lifted SPU files now receive the SDK's SSE4.1 setting, enabling the SIMD byte
shuffle. `scripts/build.ps1 -SpuFma -Test` additionally enables AVX/FMA3 for those
files, with a startup CPU/OS check. This option preserves fused arithmetic and
does not enable fast-math. The current local build uses it on the Ryzen 9800X3D;
1.2 million FMA/FMS/FNMS lane results match the scalar reference. The SIMD-only
carrier probe still ran around 9–10 actual FPS, so this is not a demonstrated
gameplay speedup. Builds without `-SpuFma` retain the SSE4.1 baseline.

Both launchers share generated shaders in `local/cache/rsx-shaders` across
sessions. Previously every timestamped session started a fresh cache and
recompiled the same shaders, causing repeated loading and first-use stalls.
The SDK validates cache version, stage, complete generated source and blob
length before reuse. `RSX_SHADER_CACHE_DIR` overrides the location.
The fresh warm-cache probe recorded 46 vertex/256 pixel shader hits with no
misses or rejected entries at that checkpoint. The final carrier probes report
zero duplicate presents and texture decode failures, and movement across the
deck still works. Native captures remain under `local/runs/20261011-191729-483`
(before/after movement: `requested_012035`, `requested_012294`) and
`local/runs/20261011-192350-729` (persistent-cache verification). The cache can
still fill during a longer scene and retains fence-safe LRU eviction. The host
FIFO idle wait was reduced from 4 ms to 1 ms for label dependencies; these
changes have not established an acceptable steady gameplay rate or eliminated
all visible artifacts. CPU simulation/rendering performance needs further work.

## Native performance investigation

The current target is a 30 FPS ceiling with dips acceptable, improved pacing,
and resolved rendering bugs. A firsthand [PlayStation LifeStyle review](https://www.playstationlifestyle.net/2009/01/03/ps3-review-haze/)
reports 30 FPS with rare drops in large encounters, while
[GameSpot's review](https://www.gamespot.com/reviews/haze-review/1900-6191308/)
describes few frame-rate jitters. These are qualitative retail observations;
neither supplies frame-time data for the opening carrier or co-op. A verified
hardware capture of the matching scene would make a stronger reference.

Native profiling on October 11 identified CPU texture hashing, vertex conversion,
inactive memory diagnostics and SPU image fingerprinting as costs. GPU timestamps
measured about 1.4–1.7 ms of execution per carrier frame. Full-content hashing now
uses independent lanes; vertex conversion batches common formats; inactive PPU
memory probes take a shorter path; SPU fingerprints reuse an exact byte-validated
cache. PUT/label writes also wake the FIFO consumer. Bounds, reservation writes
and complete content validation remain active.

Exploratory 600-frame carrier samples ranged from a 9.9 FPS baseline (p95 162 ms)
to 16.1 FPS (p95 81 ms) after the first changes. A subsequent build measured
11.7 FPS in another interval, despite similar GPU cost. Scene activity and timing
vary across these sessions, so these numbers do not establish a controlled
end-to-end speedup. **30 FPS and consistent pacing are not achieved.** The live
FIFO timeout comparison did not justify replacing the normal 1 ms wait with
busy polling. A bounded, full-source-validated vertex payload cache is available
experimentally with `RSX_VERTEX_CACHE=1`; normal builds leave it off pending
native A/B validation.

A later 600-frame carrier interval (frames 23528–24128, run
`20261011-205425-998`, vertex cache off) measured 11.37 FPS, 87.96 ms mean,
120.30 ms p95 and a 796 ms maximum. Average draw preparation was 36.66 ms,
including 13.94 ms vertex packing, 4.29 ms pipeline preparation and 4.31 ms
texture hashing. GPU execution averaged 1.49 ms. No shader compilation occurred
in that interval, so shader compilation cannot explain those sampled stalls.
The cache had reached 4,096 entries and recorded 2,207 actual replacements in
the interval. These measurements identify remaining work; they do not demonstrate
a steady rate or an end-to-end improvement.

A longer session exposed repeated failures to decode an unsupported D16 depth
texture after texture-cache saturation. Each binding previously scanned all
4,096 LRU candidates again, recording over a million failed attempts. Failed
descriptors are now retained even when the cache is full, with at most one retry
per later frame so temporarily unavailable data can recover. Regression tests
exercise saturation and repeated bindings. This bounds the overhead; D16 guest
texture decoding itself still needs implementation.

The renderer now implements the polygon offset state Haze actually submits for
overlapping surfaces. Supported GPUs use dynamic D3D12 depth bias; older devices
use static PSO state. Synthetic WARP readbacks check coplanar decals with positive,
negative and zero bias. A subsequent 40-frame temporal comparison kept all
sampled nearby yellow paint visible with bias enabled and lost it with bias
disabled. This validates that deck region; other visible artifacts remain.

For a live polygon-offset comparison, set `RSX_BIAS_FILE` to a local text file
containing `1` (normal) or `0` (disabled). It is checked every 60 presents and
state changes are logged. The temporal comparison aligns camera motion using
unpainted deck and evaluates the same interior paint mask in both clips.
Restore `1` after diagnostic comparisons.

The [controlled carrier experiments](docs/performance-2026-10-11.md) record exact
frame ranges, CPU stages and rejected defaults. Bulk reference construction
reduced its sampled cost from about 3.3 to 1.5 ms; direct dense remapping saved
several tenths more. Both now run normally. The FIFO consumer also preserves
synchronized callback ownership before callback entry, repairing a reproduced
wrap race. The subsequent [SPU storage comparison](docs/performance-spu-storage-2026-10-11.md)
found repeatable gains from avoiding large per-job stack growth: heap blocks
measured 20.25–21.36 FPS against 18.72–19.64 FPS for stack storage, with lower
CPU use and p95 frame times. Heap contexts are now the Windows default. The
experiment also exposed and fixed duplicate reservation-holder registrations
that could leave dangling pointers after cleanup. The 30 FPS goal and consistent
pacing remain open. Alternative texture hashing and timer waiting remain diagnostic
options; private SPU diagnostic history fixes a race without a demonstrated FPS
gain. The [targeted SPU byte-operation experiment](docs/performance-spu-bytes-2026-10-11.md)
passed 2.36 million independent oracle comparisons and lowered process CPU use
in three pairs, but showed no repeatable FPS or pacing gain. The
[coherent row-blit comparison](docs/performance-row-blits-2026-10-11.md) reduced
blit CPU time from about 2.45 to 0.33 ms/frame. Its row-copy blocks measured
23.55-24.50 FPS; NPC variation prevents assigning the full FPS difference to
that change. Row copies are now enabled for eligible transfers.
The [frame timeline follow-up](docs/performance-frame-timeline-2026-10-11.md)
separates live SPU comparison code from ordinary profiling builds and records
an additional carrier run without job logging. Its 19.55 FPS result shows why
the earlier 23.55-24.50 FPS windows are not a stable performance guarantee.
A fresh ordinary-build baseline measured 20.68 FPS over 600 consecutive
successful presents, with p95 frame time 63.90 ms.
The [specialized vertex-loop comparison](docs/performance-vertex-loops-2026-10-11.md)
then saved another 1.15–1.28 ms/frame of decoding, with specialized blocks at
23.16–25.22 FPS. Those loops are now enabled by default.
**The revised target is a 30 FPS ceiling with dips acceptable.** Reaching
that ceiling during gameplay and avoiding unnecessary stalls remain open.
The [depth-clipping investigation](docs/rendering-depth-clip-2026-10-11.md)
reproduces a close-barrel layering error and adds guest-controlled depth clipping;
later close-up captures also verify opaque soldiers. The
[fragment-window investigation](docs/rendering-window-position-2026-10-11.md)
reproduces camera-dependent rectangular lighting on the carrier wall and
ground. Honoring Haze's bottom-origin fragment coordinates removes those
boundaries in repeated same-process comparisons and is now enabled by default.
The full regression suite passes, including 3,072 GPU coordinate readbacks.
Broader level coverage and an isolated performance comparison remain open. The
[shadow comparison investigation](docs/rendering-shadow-compare-2026-10-11.md)
tracks Haze's observed projected depth lookups and a GPU-tested comparison
experiment, currently disabled pending carrier checks. The
[fragment depth-export investigation](docs/rendering-depth-export-2026-10-11.md)
adds GPU-tested output for Haze's depth-reduction shaders, also experimental.
The
[vertex-program cache experiment](docs/performance-vp-cache-2026-10-11.md)
reduces shader preparation time, but remains disabled because its profiling
comparison did not establish a reliable FPS gain.
The [triangle-index upload investigation](docs/performance-triangle-indices-2026-10-11.md)
measured a repeated 0.85–0.88 ms/frame saving from copying validated triangle
remaps in bulk. This is now enabled by default; overall pacing still varies.

For reproducible diagnostic captures:

```powershell
.\scripts\build.ps1 -SpuFma -ProfileRenderer -Test
$env:RSX_PROFILE_LIGHT = '1'  # Omit the legacy shader-variant tracking overhead.
$env:RSX_PROFILE_CSV = 'renderer.csv'
$env:RSX_PRESENT_CSV = 'presents.csv'
$env:HAZE_PUMP_PROFILE = '1'
.\scripts\run.ps1 -Seconds 600 -Uncapped
# Load the carrier; verify the scene before selecting a frame interval.
.venv\Scripts\python.exe scripts/summarize_profile.py local/runs/<run>/renderer.csv --last 600
.venv\Scripts\python.exe scripts/profile_native_cpu.py <HazePID> 20
```

CPU timing scopes overlap: draw preparation includes vertex conversion and
texture work. Instruction samples include waiting threads; they are not CPU
utilization percentages. Per-thread CPU counters are reported separately. Windows
ETW profiling was unavailable under the machine's performance-profiling policy,
so the native sampler briefly reads thread instruction pointers and resumes each
thread before resolving symbols from the matching PDB.
The sampler also recognizes the executable's ordinary `vm_read32` load/byte-swap
instructions and records guest addresses only at those instructions. Address
samples are sparse; they do not establish the proportion of all memory reads.
Use `--start-frame` and `--end-frame` with the CSV summarizer to retain exact
comparison intervals. `PS3_POLLTOP` reports requested sleep durations rather
than measured wall-clock sleep time.

Diagnostic `run.ps1` sessions are **muted by default** while retaining mixing,
device-clock pacing and game audio events. Add `-CaptureAudio` to save the original
mix as `audio.f32le` (48 kHz stereo float32), or explicitly `-Audio` to hear a test.
The ordinary gameplay launcher retains sound. All captures stay under ignored
`local/runs`. Rebuild without `-ProfileRenderer` for ordinary play.
Native play defaults to a **30 FPS maximum**. Slow frames present when ready;
the limiter does not try to catch up with bursts of faster frames. Diagnostics
use `-Uncapped` to measure remaining capacity. `RSX_MAX_FPS=0` is the equivalent
direct-executable override; normal capped runs record their limit in the run report.
The [limiter verification](docs/frame-limit-2026-10-11.md) found no interval
shorter than 1/30 second across 9,575 consecutive presents. A lighter carrier
view reached 29.41 FPS, while the busy deck remained at 20.70 FPS in that run.

## Local development

Select a custom internal resolution in the live renderer:

```powershell
.\scripts\play.ps1 -RenderResolution 1920x1080
.\scripts\play.ps1 -RenderResolution 2560x1440
.\scripts\run.ps1 -Seconds 120 -RenderResolution 1777x997 -CaptureFrames 2
```

`RSX_RENDER_RESOLUTION=WIDTHxHEIGHT` also works with either launcher or the
executable directly. The launcher parameter takes precedence. Omit both to
retain the original render sizes. Width and height accept integers from 1 to
8192, subject to GPU memory and texture-size limits. Restart to change them.
This option requires the Live renderer.

Haze's observed main scene is 1024x576, followed by a 1280x720 output pass.
Custom mode renders both at the selected dimensions and scales intermediate
color/depth targets relative to the scene. For example, 1920x1080 gives a
1920x1080 scene, 960x540 half-size effects, and a 1920x1080 output. This also
increases shadow-target sizes and GPU memory use. Guest memory, texture assets,
camera projection and HUD proportions retain their original conventions;
selecting a different aspect ratio stretches the picture. Ultrawide projection
and HUD adaptation remain separate work.

See [custom resolution implementation and validation](docs/custom-render-resolution.md)
for the rendering changes and native evidence.

Requirements: Windows x64, Visual Studio C++ tools and Windows SDK, Clang,
CMake, Ninja, Python 3.11+, Git, and 7-Zip. The build script discovers the
Visual Studio tools with `vswhere` and initializes their environment.

Game files, keys, downloaded helper tools, generated game translations and
build outputs stay local and are excluded from Git. Supply your own game files.
The source ISO is never modified. No PS3 game or firmware files are in this repo.

```powershell
git submodule update --init --recursive

# The supplied USA ISO is already readable at the disc level:
.\scripts\extract.ps1 -Iso 'S:\PS3\Haze (USA) (En,Fr,Es).iso'

# After supplying the game files:
python -m venv .venv
.venv\Scripts\python.exe -m pip install -r external/ps3recomp/requirements.txt

# Expected files: game/disc/PS3_GAME/PARAM.SFO and USRDIR/EBOOT.BIN.
# Decrypt the retail executable with a local official RPCS3 binary.
# RPCS3 is only a development helper; the target remains a native port.
.\scripts\decrypt.ps1 -Rpcs3 'C:\path\to\rpcs3.exe'

.venv\Scripts\python.exe scripts/haze.py inventory
.venv\Scripts\python.exe scripts/haze.py analyze
.venv\Scripts\python.exe scripts/haze.py lift --jobs 4

# SPU translation, including executable cellOvis overlays.
.venv\Scripts\python.exe scripts/haze.py lift-spu

# Translate the audio plug-ins at Haze's observed local-store allocations.
.venv\Scripts\python.exe scripts/lift_audio_plugins.py
# Requires locally installed PS3 firmware; no firmware is downloaded or bundled.
$env:PS3_DEV_FLASH = 'C:\path\to\dev_flash'
.venv\Scripts\python.exe scripts/lift_firmware.py

.\scripts\build.ps1 -RuntimeOnly
.\scripts\build.ps1

# Interactive development build (experimental):
.\scripts\play.ps1
# Or restrict input to SDL-supported controllers:
.\scripts\play.ps1 -InputMode Controller

# Synthetic SDK regression checks (no game execution):
.\scripts\build.ps1 -RuntimeOnly -Test

# Bounded native boot diagnostic; capture later frames after initialization:
.\scripts\run.ps1 -Seconds 90 -CaptureFrames 4 -FrameSkip 3000 -FrameEvery 300
```

The scripts emit binary inventory, import coverage, loader information, and
translation provenance under ignored local directories. Import coverage is a
static inventory of symbols and context registrations; it is not an ABI or
behavioral compatibility test.

Graphics are configured in [config/graphics.ini](config/graphics.ini). Restart
after changing windowed/fullscreen/borderless mode, window size, monitor,
render resolution, FXAA, anisotropic filtering (up to 16x),
independent shadow resolution, mip bias, sharpening, exposure, VSync or the
frame cap. Defaults use borderless mode at the primary monitor's desktop
resolution and retain native shadow size. Windowed mode automatically fits a
16:9 window up to 1280x720 to the monitor's work area.
Missing settings or an absent default INI use those same defaults. Both launchers accept
`-GraphicsConfig path/to/settings.ini`; `-RenderResolution 1920x1080` overrides
the file for that run. See [graphics settings](docs/graphics-settings.md) for
costs, examples, validation and remaining game-side options.

PC input is configured in `config/input.ini`, which reloads while running.
**Settings > Control Setup** opens the PC Controls page for keyboard, mouse
and controller bindings, sensitivity, deadzones and prompt style. Select a
binding to capture an input, review conflicts, then **Apply** to save and use
it immediately. **Back** discards staged edits; **Defaults** also requires
Apply. See [Controls settings](docs/control-settings.md) for supported inputs,
device mappings and validation status.
The launcher defaults to automatic keyboard/mouse and SDL gamepad input.
WASD moves; arrows navigate;
Space confirms/skips; Enter starts; Escape pauses and releases the mouse.
Click inside the game or press F1 to capture the raw mouse; F1 also releases it.
The initial capture click does not fire. Short keyboard and mouse-button taps are buffered for
one host report so they survive slow frames without stretching releases. All sixteen controller buttons have configurable
keyboard/mouse bindings. Mouse buttons affect the game only while captured,
and focus loss releases capture and neutralizes all input.
Raw device counts rotate the player yaw/pitch
before the game builds its aim and camera matrices. Mouse motion does not drive
the right stick, so it has no stick deadzone, acceleration, smoothing or
turn-speed cap. Sensitivity is degrees per count (default `0.10`), and `InvertY`
reverses vertical motion. Camera pitch still respects the game's limits.
Kenney prompts follow the last active keyboard/mouse or controller family and
the current bindings. PNG edits and INI saves reload within half a second.
`[Prompts]` selects the icon directory or restores original prompts.
`HAZE_INPUT_CONFIG` selects another configuration file. SDL supports hotplug and custom device mappings; controller
buttons, axes, deadzones, trigger threshold and look inversion also live in the
INI. See [controls implementation and verification](docs/controls.md).
The main menu offers **Quit to Desktop / Add to Steam**. Click the footer or use
the current Select binding (Tab by default) to open Desktop Options. Steam
registration requires selecting a local account and confirming the launcher;
exit Steam first. The executable launcher and its embedded icon are still
dependencies tracked in #42 and #41. See [Steam integration](docs/steam-integration.md)
for setup, local artwork, checks and remaining acceptance.

The pause menu offers **Quit to Desktop** directly. Click the action or use
Select (Tab by default); its confirmation defaults to Cancel. Quit waits for
active saves/settings before stopping native output. Ordinary window-close
requests use the same coordination once the guest frame loop is running.
See [Quit to Desktop](docs/quit-desktop.md) for behavior and validation status.

From an isolated worktree, share the existing SDK, lifts and game assets while
keeping build products and run logs in the worktree:

```powershell
.\scripts\build.ps1 -SharedRoot E:\Code\haze-recomp
.\scripts\test-input.ps1 -SharedRoot E:\Code\haze-recomp
.\scripts\run.ps1 -SharedRoot E:\Code\haze-recomp -Seconds 600
.\scripts\play.ps1 -SharedRoot E:\Code\haze-recomp
```

For renderer changes, `build.ps1 -SdkRoot .\external\ps3recomp` selects an
independent SDK checkout while `-SharedRoot` continues to supply the lifts and
game assets. The selected SDK must be at a compatible revision.

`HAZE_MOUSE_TRACE=1` logs raw counts and the resulting angles after the game's
camera update for diagnostics. The native hook checks the supported ELF's
instruction and virtual-function signatures before accessing player angles.
Input regressions cover raw relative events, neutral right-stick packets,
all sixteen buttons, tap buffering, focus/capture release, distance invariance,
large flicks, pitch limits and inversion. The isolated executable built and
booted into the campaign; a Windows raw-input probe measured the configured
angle changes, and interactive testing confirmed native mouse feel.

The PC defaults were checked against the running game's **Customize Controls**
page with its Default layout:

| Action | PC default |
| --- | --- |
| Move / look | WASD / mouse |
| Fire / weapon zoom | Left / right mouse |
| Reload / use | R / E |
| Jump / crouch | Space / Ctrl |
| Switch weapon | X or mouse wheel |
| Throw grenade / Nectar or play dead | G / Q |
| Melee | F |
| Menu confirm / back / start or pause | Space / X / Enter or Escape |

Haze defaults to toggle zoom and crouch. **Game control options** at the bottom
of the PC Controls page opens the original menu to change those to held input.
Selecting a different controller layout there
also changes the actions associated with the PC button bindings.

The Ubisoft, Free Radical and menu attract videos currently have no decoder.
The port automatically presses the game's skip button while those exact streams
are active, then cancels it when the player stops/closes. This fixes the black
screen after the logos without requiring manual skip presses or diagnostic
input files. Story videos are excluded. A fresh native launch reached the
rendered Press Start screen without any injected diagnostic input; Enter is
still required to start. Skip retries stop after ten seconds if the title does
not respond. Decoded movie playback remains unimplemented.

## Initial findings

- Retail SELF decrypted to a 16,039,384-byte ELF.
- Entry descriptor `0x00e35fb0`, entry code `0x00010230`, TOC `0x00e94ee0`.
- 44,520 OPD functions; the first PPU lift generated 58,637 functions.
- 216 firmware imports across 14 libraries.
- 44 embedded SPU ELF images; several produce unsupported `.word` warnings.
- SPU image at ELF offset `0x00d15f80` has multiple executable segments and
  overlays. The fork now selects the segment containing entry `0x200f0`,
  fixing the missing entry symbol. Synthetic segment and overlay checks pass.
  Overlay execution and unsupported instruction/data classification still
  need further work.
- The upstream runtime compiled locally with Clang 20.1.8.

## Verified local progress and current blockers

Opening-logo skipping is configured in `config/input.ini` under `[Startup]`.
`AutoSkipLogos=1` is the default; `0` waits for the current Cross/confirm binding
(Space by default, or the rebound controller button). The media backend cannot
decode these movies yet, so this is **not** a watch-logos mode. Idle attract-demo
skipping is controlled separately by `AutoSkipAttract`; campaign movies are
excluded. See [startup skipping](docs/startup-skipping.md) for behavior and validation.

The latest native probes verified unattended startup through the undecoded
branding videos to the Press Start screen, then loaded the campaign through
its normal menus. A continued checkpoint allowed walking across the carrier,
camera rotation, NPC animation and pause/resume. A separate New Campaign probe
used an isolated copy of the saves, played the opening cinematic without
injected input, reached and saved its first checkpoint, and completed
**Gather with the rest of your squad** after walking into the group. The green
objective-complete notification was captured. Both campaign probes closed
cleanly with exit code 0. Combat and subsequent mission progression remain
unvalidated. Keyboard/window-message and raw-mouse mapping regressions pass;
these campaign probes used diagnostic pad input, so physical mouse/keyboard
feel still needs an interactive check. No game or firmware assets were added
to Git.

The Windows executable is `build/haze/HazeRecomp.exe`. Native boot probes
create PPU worker threads, execute lifted SPU jobs, read the game archives,
configure GCM display buffers, and submit D3D12 draw calls. A 120-second probe
completed roughly 1,700 SPU jobs and reached repeated pad polling and frame
submission. The live D3D12 renderer subsequently displayed Haze's splash/logo
and ESRB notice. After adding the deprecated SAIL lifecycle ABI, native tests
can skip the Ubisoft, Free Radical and attract movies with Cross and render
the Press Start title screen. The first-run installer now writes 72 files
(about 4 GB) to its local game-data USRDIR, and a subsequent boot passes the
installed-data check without reinstalling. Captures show the interactive main
and Campaign menus, including Single Player Campaign, Join Co-op and Host Co-op.
Subsequent fixes reach the controllable opening carrier scene, as described
below. Mission completion and co-op remain unvalidated; a diagnostic timeout
is not a successful gameplay test.

The SDK fork on its `haze` branch includes the executable-segment fix, Windows
DXGI GUID linkage, corrected game-data callback structure layout and first-run
directory status, and a fix that makes the unresolved-import error policy take
effect. Fixing the callback removed the corrupt-game-data dialog in a native
boot test. The installer also required 64-bit Windows stat/seek operations:
its 4,003,576,161-byte `install.qik` otherwise incorrectly appeared absent.
The fork creates the callback-approved game-data directories and persists
their PARAM.SFO metadata. Tests cover canceled checks, missing parameters,
metadata round trips, and actual stat/seek/open operations on an 8 GB sparse
fixture. Profile/checkpoint persistence is now verified below.

Retail memory-container calls now route through syscalls 341/342, with 350
allocating from a real tracked container. The allocator reuses freed ranges
and protects mmapper reservations from overlap. This fixes invalid container
IDs and exhaustion after repeated startup buffers. Regression checks exercise
the syscall table, capacity and lifetime errors, 300 allocate/free cycles,
and reservation isolation. Campaign loading is still under investigation.

New Campaign now reaches the first mission's loading screen. Physics setup
required the retail one-argument overlay-size API and a separate staging buffer
for ELF segments whose physical addresses exceed SPU local store. The loader
preserves resident segments instead of overwriting them with overlays. Tests
cover staging copies, alignment, bounds, BSS and overlay isolation. SPURS task
errors now use the retail `0x804109xx` facility; the previous empty-queue code
made Haze treat a normal retry as fatal. Queue pop now reads Peek and blocking
as separate third and fourth arguments; a delayed-producer regression verifies
that blocking pop waits for data. ELF constructor/destructor arrays also seed
SPU function detection, including one-entry tables the heuristic missed.
The physics overlay is compiled separately and bound to its relocated staging
buffer by its complete content fingerprint. Synthetic checks cover overlay
selection, relocation, content discrimination and allocation reuse. Native
campaign startup advances past the first blocking queue wait. Mission
rendering, control and physics remain unvalidated.

Each SPU context now owns its DMA command queue and tag-completion state.
The former eight-slot registry shared an overflow engine during Haze's worker
dispatch, allowing unrelated tasks to mix DMA state. Job exit releases the
private engine. A concurrent regression checks 32 contexts and 3,200 reuse
cycles. A separate launch race ran one task's compiled code with another
task's arguments after task-ID reuse. CreateTask now schedules current ready
state, and asynchronous dispatch snapshots arguments before launching its
host thread. The observed alignment assertions are gone in a native campaign
probe, but startup still waits for a later physics completion. This has not
established playable campaign gameplay.

The overlay compiler now discovers entries called from other executable
segments. Interpreter fallback, indirect calls and call returns use the same
resident-code ownership rules. For BLUS30094's small-island physics overlay,
the build limits that ownership to the range before LS `0x17100`, where Haze
streams writable work data. Its fingerprint still covers the complete segment.
This prevents data loads from evicting its compiled instructions. Regression
checks cover tail writes, continued code loads and actual code overwrites.
The SPU instruction tracer now reports the actual destination of four-operand
instructions, including shuffle and select operations. A regression checks all
six encodings. A corrected campaign trace follows the empty object to a DMA
list whose source is `0x80`; the physics allocator returns that invalid address
before the list is built. This was caused by the SDK writing 12 bytes into
Haze's eight-byte lightweight condition variable, overwriting the adjacent
allocator callback. The corrected handler stores two 32-bit fields, matching
the [retail ABI structure](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/sys_lwcond.h).
A regression runs the actual handler and checks neighboring memory, callback
preservation, queue IDs, and mutex release/reacquisition during wait. Native
campaign probes now retain the correct allocator callback and advance without
the observed invalid DMA or null SPU physics handler.

The subsequent PPU object-iterator stall came from a corrupted component cache.
The lifter omitted `vsl` and `vsum4ubs`, which Haze uses to invalidate cache ages
and update its replacement order. Duplicate cache entries eventually produced
invalid component pointers. Both instructions now have native implementations;
compiled regression cases check 2,496 shift operations and 6,018 saturated sums,
including register aliases and sticky saturation state. A local fixture using
the game's cache routines fails on lookup 16 before the shift fix and passes
200 lookups afterward. The latest native campaign run advances through the
opening credits without the observed iterator stall. Profiling found the physics
coordinator blocked in synchronous log writes: the SDK disabled the runner's
stdout buffering and emitted each task dispatch even in quiet mode. Quiet runs
now retain buffering and suppress those success logs. A native campaign probe
improved from below 1 FPS to roughly 16–27 FPS during its opening sequence.
These are local diagnostic rates, not a validated gameplay benchmark. Optional
`SPU_JOB_TIMING=1` reports setup and execution wall times. The runtime regression
suite passes. The opening title and credits render, but subsequent captures are
black despite scene draw submissions; rendering is the next blocker. Campaign
control and co-op remain unvalidated.

Campaign probes also exposed a task-slot publication race: completion could
clear an enabled bit while another creator restored it from a stale read,
eventually exhausting all 128 slots. Descriptor publication and slot selection
now share the completion lock. A regression exercises 40,000 concurrent
create/exit cycles. The renderer's fixed 256-packet draw storage also discarded
large batches; growing storage passes 5,000-packet ordering and reuse checks,
and a native probe submitted over 14 million packets with no capacity drops.

Raw shader captures identified another rendering fault: many GPU fragment
programs contained patched constants but zero instruction bytes. Haze copies
these programs through NV0039, which the SDK previously ignored. The fork now
executes unit-stride memory-to-memory transfers between mapped main memory and
local GPU memory, and keeps 2D-engine methods out of the 3D register stream.
Tests cover byte preservation, signed pitches, overlap, trigger sequencing,
and invalid ranges. The register contract is documented in
[envytools](https://github.com/envytools/envytools/blob/master/rnndb/graph/nv_m2mf.xml).
The subsequent native capture's first 252 programs have no empty initial
instructions or malformed decoded programs, with no observed PSO failures.
The scene still renders black; vertex output, render state, and the
postprocessing chain remain under investigation. This is not playable yet.
Non-unit transfer increments and notification DMA remain unsupported and
produce diagnostics rather than fabricated completion.

The live renderer also binds Haze's 16-bit depth render targets for sampling;
previously only 24-bit depth textures used the GPU result. Native probes now
record thousands of depth-SRV bindings. Paired draw captures can preserve the
actual vertex inputs, shader programs and constants for independent execution.
Some captured geometry projects inside the camera frustum, and a captured
material shader produces nonzero colors with controlled texture inputs.

FIFO history exposed commands being overwritten during decoding. The reserve
callback formerly reused its buffer after a timeout, and the flip helper could
rewind it while the tail was still pending. Recycling now waits for separate
tail and wrap acknowledgments, and the callback retains ownership while waiting.
The actual callback/drain regression covers a delayed consumer, stale GET at
the ring start, flip insertion, and semaphore waits across twelve ticker passes.
The full regression suite passes. A subsequent native opening-sequence probe
advances without observed FIFO recoveries or PSO failures, but its captured
scene remains black. Campaign gameplay and co-op are still unvalidated.

Fragment predication now selects individual lanes instead of interpolating
between old and new register values. Interpolation allowed an inactive NaN or
infinity to corrupt the selected result and turn it black. An asset-free test
executes the generated shaders through D3D11 WARP: the original implementation
fails, while the corrected version passes with 16-bit and 32-bit registers,
both with normal and IEEE-strict compilation. The build's test mode runs this
check and the existing 36 fragment-decompiler checks. This fixes a verified
shader defect; it did not by itself restore the final native scene.

Complete-frame draw captures subsequently showed the squad and environment
surviving Haze's initial scene resolve, then disappearing at an alpha-only
clear. The renderer had cleared all four color channels regardless of the
RSX channel mask, and ignored the clear's scissor rectangle. Clears now preserve
unselected channels and pixels outside the rectangle, including floating-point
targets. An asset-free D3D12 WARP regression runs the actual clear implementation
and checks every channel of RGBA8 and RGBA16F readbacks. The normal native
renderer now displays the opening carrier scene with the squad, weapons and sky;
the diagnostic run reports roughly 16 FPS. This establishes scene rendering,
not playable campaign completion or enhanced frame-rate support.

The runner now keeps saves in `local/state/savedata`, independently of each
diagnostic run's working directory; `PS3_SAVEDATA_ROOT` overrides that location.
The port preserves missing-load errors so Haze creates its initial profile
instead of treating an absent profile as successfully loaded. Native writes
now include profile/checkpoint data and metadata, and later writes recognize
the directory as existing. A regression checks missing-load behavior and an
actual metadata/payload round trip after changing the working directory.
A subsequent native boot successfully loads both files, enables Continue
Campaign, and resumes the carrier scene. Bounded analog-input probes visibly
turn the camera and move the player along the deck. These tests establish
opening-scene input and persistence; they do not establish mission completion.

The SDK now exposes a port-owned pad-input callback. Haze uses it for its
keyboard/raw-mouse mapping and the SDL3 controller wrapper. Regressions
exercise actual guest pad packets, pressure values, neutral release, mouse
delta consumption and focus/capture gating. A native window-close probe exits
with code zero. Interactive mouse feel still needs validation.

Audio device queries had four-byte sound modes at offset 6, while Haze reads
eight-byte modes starting at offset 8. Correcting the device/mode ABI, device
availability value and output-state offsets lets Haze initialize cellAudio and
start its mixing ports. Byte-layout, endian and boundary regressions pass.
The audio worker also loads position-independent programs at runtime: an
ATRAC firmware decoder and embedded game DSP plug-ins. Local generators now
translate the observed allocations, preserving indirect entries discovered
at the original link addresses. The runtime matches complete segment content
inside padded DMA blocks, including multiple resident programs and reused
staging buffers. Native menu probes now produce nonzero mixer samples without
the earlier unlifted-code termination. Sound quality and all campaign audio
paths still need validation. Additional allocation locations may need native
translations as more missions are exercised.

A subsequent clean native probe resumes the saved carrier scene, displays the
first squad objective, and responds to camera/forward input while producing
nonzero audio throughout. Observed frame rates vary with the view: roughly
14–34 FPS, with temporary drops into single figures during this test. A prior
run with heavy local-store dumping exhausted the physics task IDs and exited;
the clean probe has not repeated that failure. Stability, combat and mission
completion still need testing.

`scripts/lift_firmware.py` currently validates the USA build's decoder allocation
at base `0x33100`. It uses `PS3_DEV_FLASH`, an explicit `--dev-flash`, or the local
development firmware directory. The launchers also honor `PS3_DEV_FLASH`.
Firmware, extracted plug-ins and their generated translations stay ignored.

SPURS queue metadata now matches the lifted SPU consumer. PPU push/pop copy
payloads, update ring indices under the shared atomic lock, and signal SPU
waiters. A taskset scheduler scans for children created by persistent SPU
tasks, allowing Haze's controller to run its worker jobs. The DMA repeat
watchdog is disabled by default: repeated transfers are legal, and the former
default incorrectly halted a polling worker. Its optional diagnostic state is
now per SPU context. Synthetic tests cover these contracts, including queue
wrap/full/empty cases, task reuse and dispatch failure, and watchdog isolation.

The live native D3D12 renderer now displays the Haze splash/logo screen and
ESRB online notice. The legacy renderer displays the sky when its pass is
isolated, but the complete postprocessing chain goes black. The port defaults
to the live renderer; `RSX_LIVE_DRAW=0` or the diagnostic runner's
`-Renderer Legacy` selects the older path. A sample-projection heuristic no
longer overwrites Haze's valid negative matrix coefficient. Missing imports
observed during boot include
`cellSpursGetSpuThreadId` and `cellPadInfoSensorMode`. The deprecated
`cellSailPlayerInitialize` now has a separate ABI handler, including its ninth
stack argument. Player callbacks and adapter lifecycles let Haze use its own
movie skip path. Frame queries return explicitly empty frames; media decoding
is not implemented. Tests cover callback packing, object boundaries, slot
reuse, and empty-frame output. SPU overlays and unsupported instructions also
remain open.

Haze's executable contains Demonware lobby references and Ubisoft auth/STUN
hostnames. Peer-to-peer co-op will require tracing that game-specific session
path and replacing service dependencies where necessary. Adding a generic
socket transport by itself does not connect Haze's campaign state machines.

`scripts/run.ps1` returns errors for unregistered firmware imports and writes
logs, milestone counts, shader dumps, and optional frame captures to a unique
`local/runs/<UTC timestamp>` directory. `local/boot-result.json` points to the
latest run. `-FrameSkip` and `-FrameEvery` select later frames. The runner
terminates its process after the chosen timeout; `gameplay_validated` remains
false. The executable and runner default to `PS3_VERBOSE=0` because verbose
SDK logs cause thread contention and severely slow native execution; set it
to `1` for detailed diagnostics. Live-renderer captures use PPM; legacy
captures use BMP. Boot stack
reports are best-effort diagnostics. Queue operations no
longer require the upstream experimental environment flag.

## Experimental browser build

The `feat/wasm-browser` worktree builds a Wasm64 runtime and a WebGL2 backend.
Browser checks have verified the aligned guest memory arena, pthread events and
joins, lazy local disc reads, and HLSL-to-GLSL translation with textured GPU
rendering, RSX texture channel remapping, depth attachment compatibility, and
visible canvas pixels. Browser PCM playback passes a shared-memory AudioWorklet
test at 48 kHz. Haze reaches its title screen with readable text and game audio;
keyboard Start advances into its menus. Campaign assets load and the opening
credits run. A campaign checkpoint renders its 3D scene and post-processing;
WASD movement, mouse look, and pause/resume have been verified in Chromium.
The synchronized build measured approximately 5–9 FPS across two carrier-deck
views on the test machine (RTX 5080 with Chromium/ANGLE). Synchronizing the native
branches and retaining GPU presentation have not resolved the low frame rate;
performance is still experimental. Full campaign progression has not been
validated. See [browser integration and measurements](docs/browser-sync-2026-10-12.md).
Browser SPU jobs use bounded stacks and heap contexts to avoid exhausting linear
memory. The backend reuses compiled shaders across pipelines and optimizes their
register arrays while preserving guest constant-buffer layouts. Texture units
use independent cached sampler objects, including when they share an image.
Color clears preserve guest channel masks and scissor rectangles, including
alpha-only clears between post-processing passes. Texel-addressed textures
scale coordinates by their image dimensions.
Passes that sample their own render target use a reusable GPU snapshot.
Resized depth images retain writes across consecutive draws and synchronize
when a clear, snapshot, fence, target change, or presentation requires it.
Presentation transfers a GPU compositor copy while retaining WebGL's drawing
buffer, avoiding the recurring CPU pixel-readback fallback.

From that worktree, reuse the generated PPU/SPU sources and local assets in the
main checkout:

```powershell
py -3 scripts/bootstrap-wasm.py
.\scripts\build-wasm.ps1 -Emsdk E:\Code\ra2-web\.deps\emsdk
py -3 scripts/serve-wasm.py
```

Open <http://127.0.0.1:8085/> in a current 64-bit Chromium browser. The
game fills the browser window with no log or buttons. Click anywhere to enable
sound and request browser fullscreen; browsers require that first gesture.
Add `?debug=1` for the controls and log, or use F11 for browser fullscreen.
The local server automatically connects `game/disc` and `local/tools/rpcs3/dev_flash` from
the main checkout, and opening the page boots the decrypted `EBOOT.ELF` directly.
It serves asset byte ranges on demand and binds only to localhost. Override the
paths with `--game-dir` and `--firmware-dir`; add `?manual=1` to choose folders
instead. Existing installed data under `game/disc/game/BLUS30094` is read directly
from those local assets, avoiding a second 4 GB installation. Build and game
data stay in ignored directories. Browser saves use IndexedDB, separately from
native saves; when no local installation is present, HDD data also uses IndexedDB.
The game's profile save was verified by changing a sound setting, accepting its
save confirmation, and checking that the new profile bytes survived a page reload
without a manual filesystem sync. This used an isolated test browser profile.

WASD, mouse look, and keyboard menu controls have been exercised in-game.
Standard gamepads have a browser bridge but have not been tested with hardware.
Click the canvas to capture the mouse and press F1 to release it. WASD moves,
Enter opens Start/pause, Space confirms/jumps, X goes back, and arrows navigate
menus. The diagnostic page lists the remaining mappings. Browser mouse counts
use the same distance-based camera hook as the native port, retaining motion
until the simulation consumes it. The game's audio clock continues at 48 kHz
while browser sound is suspended.

Use `?smoke=1` for the runtime check, `?smoke=gl` for the GPU check,
`?smoke=audio` for PCM playback (build target `HazeAudioSmoke`), or `?trace=1`
for periodic guest thread reports. `scripts/playtest-wasm.py` records logs,
screenshots and an explicit gameplay-validation status under `local/wasm-tests`.
Its `--sound`, `--capture-interval 30`, and `--profile-after 120` options unlock
sound, retain periodic canvas captures, and sample browser worker CPU activity.
Add `?glvalidate=1` to enable expensive per-draw WebGL error diagnostics.
Normal rendering reuses framebuffer attachments and checks errors at presentation.
The playtest's `--input-file` accepts timed keyboard actions plus `runtime_evaluate`
and `gpu_evaluate` JavaScript functions for inspecting the module owner or render
worker. Calling `runtime._haze_wasm_gpu_debug(4)` captures one frame's draw targets
as small PPM files; `--trace` retrieves those files and the generated shaders.
Debug bits 1 and 2 temporarily disable depth testing and culling respectively;
pass 0 to restore guest state. These options are for renderer diagnosis.
The Wasm build requires shared memory, cross-origin isolation headers from the
included server, and around 8 GiB of addressable linear memory. Its shader
dependencies are pinned by the bootstrap script. Unsupported fiber and thread
suspension APIs return errors; browser network play is not implemented.

## Development order

1. Resolve build, SPU segment selection, and executable boot failures.
2. Reach menus and correct single-player campaign gameplay with graphics,
   audio, timing, input, and saves.
3. Trace and implement Haze's session/authentication and peer traffic, then
   test a campaign session across two machines and different networks.
4. Add mouse/keyboard integration, prompts, frame-rate changes, and ultrawide
   projection/HUD fixes. Test gameplay timing and co-op after timing changes.

The SDK and port have separate histories. Commit and push source milestones;
never publish decrypted binaries or generated game translations.

The port scaffold derives from ps3recomp's MIT-licensed project template;
the original license and attribution are retained in [LICENSE](LICENSE).
