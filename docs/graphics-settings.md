# Graphics settings

Automatic monitor defaults from `10b87ee` are merged into main, alongside
the graphics controls from `1bc4030`, native mouse input and rendering fixes.
SDK merge `45faa42` retains both SDK histories and the measured triangle-index
improvement; automatic defaults require no additional SDK changes.

Edit `config/graphics.ini`, then restart the game. The launchers resolve its path
before starting the executable in a session directory. Use another file with
`-GraphicsConfig`; direct launches can set `HAZE_GRAPHICS_CONFIG` to an absolute
file path. Without an explicit config path, the executable looks for
`config/graphics.ini` relative to its working directory and uses defaults if
absent. An explicitly requested missing file fails startup.
The launchers also use defaults when the repository's default INI is absent;
an explicit `-GraphicsConfig` or `HAZE_GRAPHICS_CONFIG` still must exist.
Missing keys use built-in defaults; an empty `[graphics]` section is sufficient.

```powershell
.\scripts\play.ps1
.\scripts\play.ps1 -RenderResolution 1920x1080
.\scripts\play.ps1 -GraphicsConfig local/my-graphics.ini
.\scripts\run.ps1 -Seconds 180 -GraphicsConfig local/my-graphics.ini -CaptureFrames 2
```

## Available options

| Setting | Values / default | Effect and cost |
| --- | --- | --- |
| `display_mode` | `borderless` (default), `windowed`, `fullscreen` | Windowed has a title bar and resize frame. Borderless fills the selected monitor without a frame or always-on-top behavior. Fullscreen additionally requests DXGI fullscreen state; Windows may use fullscreen optimizations. If Windows refuses it, a diagnostic reports the borderless fallback. |
| `window_size` | `auto` (default), `WIDTHxHEIGHT`, axes 1..8192 | `auto` selects a 16:9 client up to 1280x720 that fits the selected monitor's work area, accounting for DPI, decorations and taskbar space. Explicit sizes set exact client pixels. Independent of internal render resolution. Fullscreen/borderless use the monitor's desktop size instead. |
| `monitor` | `primary` (default), 1..16 | Selects the primary monitor or a 1-based number from the boot log. Unavailable monitors fail startup instead of silently selecting another screen. |
| `render_resolution` | `auto` (default), `native`, `WIDTHxHEIGHT`, each axis 1..8192 | `auto` uses the selected monitor's current desktop resolution in physical pixels, limited to 8192 per axis. `native` retains the game's original sizes. Custom/automatic sizes raise raster resolution, including intermediate targets: the 1024x576 scene and 1280x720 scanout both reach the selected size. More GPU work and memory; aspect ratio changes stretch the original projection and HUD. |
| `anti_aliasing` | `off` (default), `fxaa` | Directional edge AA on the final image. Inexpensive compared with supersampling, but can soften text and fine details. Does not provide MSAA or temporal AA. |
| `anisotropic_filtering` | `1` (default), `2`, `4`, `8`, `16` | Improves oblique filtered, mipmapped color textures. Preserves point sampling, single-level/linear textures, texel-coordinate reads and depth comparisons. `1` retains the existing game filter. |
| `shadow_resolution` | `native` (default), `follow`, `1024`, `2048`, `4096`, `8192` | Sets the known 2048x2048 shadow atlas independently. `native` retains 2048 to avoid multiplying shadow memory with automatic desktop rendering. `follow` uses render scaling. A 4096 atlas has four times the native pixels, 8192 sixteen times. Existing shadow artifacts may remain. |
| `mip_lod_bias` | -2..2, default 0 | Adds a bias to eligible color samplers. Negative values request sharper available mip levels and may shimmer; positive values soften them. Cannot create additional texture detail. |
| `sharpening` | 0..1, default 0 | Final-image sharpening constrained to local color bounds to limit halos. Strong values can emphasize noise. |
| `exposure` | -2..2 stops, default 0 | Final-image brightness multiplier, including HUD. +1 doubles displayed RGB, -1 halves it. This operates on the game's display colors, rather than physically based HDR lighting. |
| `vsync` | `auto` (default), `on`, `off` | `auto` retains the existing behavior: Present interval 0 when capped, 1 when uncapped. `on` requests interval 1; `off` requests 0. Desktop composition/driver behavior can still govern tearing. |
| `frame_limit` | 0..240, default 30 | Presentation ceiling. Selecting 0 (unlimited) or 31..240 also bypasses Haze's original 30 FPS wait. Higher caps do not solve CPU bottlenecks; gameplay timing above 30 FPS remains experimental. |

A quality-oriented starting point is 1920x1080, FXAA, 16x filtering, native or
4096 shadows, mip bias 0, and sharpening 0.15. Keep exposure 0 unless you want a
brightness adjustment. GPU memory limits still apply; the largest allowed
sizes are not a performance recommendation.

All options are read at startup. F6 explicitly reloads live-capable settings;
see [live application, presets and restart requirements](video-live-settings.md).
Unknown sections/keys, duplicates, invalid
values, nonfinite floats, and invalid environment overrides fail startup with
a file/line or variable diagnostic. The complete file is validated before
settings are applied. Environment overrides take precedence over the file;
launcher `-RenderResolution` and `-Uncapped` take precedence over environment
values for their respective settings. Configured values are logged as
`[graphics]` at boot, with `auto` sizes resolved to concrete dimensions before
the renderer starts. `[display] auto` reports the monitor and detected sizes;
`[graphics] Loaded` identifies the INI, or a defaults diagnostic identifies an
absent optional file. Automatic sizing is recalculated on each launch, including
after changing the selected monitor or its desktop resolution. The settings target the native Live D3D12 renderer; the
Legacy diagnostic backend does not implement these quality controls.

For example, `display_mode = borderless` and `monitor = primary` fills the
primary desktop; `render_resolution = auto` renders at that desktop's resolution,
while `render_resolution = 1920x1080` renders at 1080p regardless of desktop size.
Use `display_mode = windowed` and `window_size = 960x540` for a smaller window
without changing the rendering resolution. Window resizing scales the fixed
internal image; focus loss releases captured mouse input. Mode changes require
restarting; DXGI's automatic Alt+Enter is disabled.

| File setting | Environment override |
| --- | --- |
| `display_mode` | `RSX_WINDOW_MODE` |
| `window_size` | `RSX_WINDOW_SIZE` |
| `monitor` | `RSX_MONITOR` |
| `render_resolution` | `RSX_RENDER_RESOLUTION` |
| `anti_aliasing` | `RSX_ANTI_ALIASING` |
| `anisotropic_filtering` | `RSX_ANISOTROPY` |
| `shadow_resolution` | `RSX_SHADOW_RESOLUTION` |
| `mip_lod_bias` | `RSX_MIP_LOD_BIAS` |
| `sharpening` | `RSX_SHARPENING` |
| `exposure` | `RSX_EXPOSURE` |
| `vsync` | `RSX_VSYNC` |
| `frame_limit` | `RSX_MAX_FPS` |

## Other options investigated

Higher-resolution textures require higher-resolution source assets and a
replacement loader. The renderer already uploads the available game mip
levels; enlarging those images would interpolate the shipped pixels. Filtering,
mip bias and render resolution can improve their presentation, but this change
does not add a texture pack or replacement pipeline.

Draw distance requires game-side geometry culling, LOD and streaming changes.
Read-only inspection of the supported ELF found `grassDrawDist`, `maxViewDist`,
`minViewDist`, `lodDist`, and `detailCullDist`. These names are leads, not proven
global switches: their access paths and per-object behavior still need tracing.
Changing the host depth clip plane alone cannot restore geometry the game did
not submit. No draw-distance setting is exposed yet.

The ELF also contains `motionBlur`, `k_playerPainBlur_doMotionBlur`, and FOV/zoom
parameters. Motion blur, field of view, per-object detail distance and grass
distance are candidates for a subsequent game-side settings pass, after
identifying safe hooks and checking cutscenes, zoom, culling and streaming.
There is no verified global film-grain or depth-of-field switch in this change.

## Renderer details and verification

FXAA, sharpening and exposure run in a private compute presentation pass. Its
source stays unchanged for later guest texture reads. The pass preserves alpha;
ordinary frame dumps and recordings capture the processed display image.
Direct movie uploads retain their existing path. When all three effects are
off, no presentation pipeline or extra image is allocated and the original
copy path remains active.

Shadow scaling uses a title-owned 2048-square atlas reference. Both color
aliases and depth snapshots of that exact raster extent use the chosen size;
viewports, scissor clears and fragment window coordinates follow the same
mapping. Guest addresses, pitches, texture descriptors and game memory are not
enlarged. Other titles must opt in with their own atlas reference.

Sampler quality changes wait for the GPU and invalidate cached samplers and
descriptor tables before subsequent draws. Cached sampler
identities include whether the texture receives an override, preventing color
settings from leaking into excluded depth/UI textures. Their API meanings follow Microsoft's
[D3D12 sampler descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_sampler_desc).

The window-owning thread uses per-monitor DPI awareness. Fullscreen transitions
wait for the GPU, release scanout references, resize buffers, and recreate RTVs;
internal render targets remain intact. A focus acquisition retries a refused
fullscreen request once, and focus-driven DXGI state changes refresh scanout
buffers before the next present. Shutdown requests windowed state. This follows
Microsoft's [D3D12 swap-chain requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains)
and [SetFullscreenState guidance](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate).

`scripts/build.ps1 -SpuFma -Test` includes configuration tests and the existing
renderer/runtime gate. Focused WARP checks exercise all filtering levels and
exclusions, independently scaled shadow attachments/scissor clears, FXAA
diagonal edge smoothing, actual sharpening within local bounds, exposure,
alpha preservation, and unchanged source images. Non-multiple dispatch sizes
exercise compute bounds. Existing window-position, masked-clear, depth and
shadow-comparison tests remain part of the gate.

Automatic sizing uses a hidden per-monitor-aware window with
[GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)
to determine the selected monitor's effective DPI, then restores the caller's
DPI context. The probe is never shown. Explicit dimensions remain unchanged.

The Win32 display test creates real windows and checks exact client pixels,
borderless rectangles on each connected monitor, restored windowed frames,
fullscreen placement and invalid-monitor rejection. This machine exposed four
monitors (three 2560x1440 and one 1920x1080). Automatic-default checks also compare
resolved rendering dimensions to each monitor and ensure the entire automatic
window, including decorations, stays inside that monitor's work area.
Configuration tests cover omitted keys, an absent optional INI, automatic and
explicit sizes, environment precedence, and failure before applying values.

Local gate logs: `local/graphics-final-build-tests.log`,
`local/graphics-display-build-tests.log`, and `local/graphics-focused-tests-final.log`.
The final display focus retry was rebuilt in `local/graphics-display-final-build.log`.

Native evidence (all with D3D12 debug enabled):

| Run | Check |
| --- | --- |
| `20261012-045803-305` | Initial 1080p/FXAA/16x AF/4096 shadows/bias -0.5/sharpen 0.25/exposure 0.25/VSync on; carrier capture `requested_002854.ppm` visually inspected |
| `20261012-050441-412` | Repeated carrier run after sampler-cache isolation fix; same quality options, effective settings recorded in JSON; captures from frame 2400 |
| `20261012-050150-750` | Defaults: native resolution, no image effects, original filtering; 1280x720 title capture visually inspected |
| `20261012-051155-815` | Fullscreen requested on primary 2560x1440 monitor with 1080p internal image; Windows refused activation (`0x887A0022`), borderless fallback and minimize/restore stayed operational |
| `20261012-051432-218` | Fullscreen requested on monitor 1 after focus-retry addition; same Windows refusal, minimize/restore and fallback checked |
| `20261012-051617-276` | Borderless on monitor 4: 1920x1080 client with independent 1777x997 rendering and FXAA |
| `20261012-051827-071` | Windowed: exact 960x540 client with native 1280x720 scanout; startup image captured |
| `20261012-051917-217` | Missing monitor 16: native startup exits with code 2 and a diagnostic, without timing out |
| `20261012-052011-653` | Final borderless candidate, later title captures at 1777x997 |

Fullscreen activation cannot be verified in this tool session: foreground
activation returns false and `GetForegroundWindow` returns null. The fallback
is verified; actual DXGI fullscreen activation and automatic fullscreen-state
transitions need an interactive desktop check. These are bounded opening-scene
and presentation checks, not campaign completion or performance benchmarks.

The complete native logs above contain no D3D12 error/corruption or
resource/presentation failure. Existing optimized-clear and unused-MRT warnings
remain. The complete final regression gate is
`local/graphics-display-final-gate.log`; the subsequent missing-monitor exit
change was rebuilt in `local/graphics-display-candidate-build.log` and verified
by its native rejection run.

Original graphics feature SDK source: `9cff4eb`. Its candidate is
`local/candidates/graphics-settings-final/HazeRecomp.exe`, SHA-256
`B7AED4331346E0EC9D87F019EC8A79ECC42B8CA2797C5237E5B0A24BFED48BB7`.
Its adjacent `Play-Haze.cmd` launches the candidate using this worktree's
`config/graphics.ini`. The worktree's default `build/haze/HazeRecomp.exe` and
PDB were updated to the same final build. The main checkout is separate.

## Main integration feedback build

`local/releases/haze-wip-2026-10-11-graphics-settings/HazeRecomp.exe` was built
from main `821868e` / SDK `45faa42`, with SPU FMA enabled and profiling and
comparison instrumentation disabled. SHA-256:
`EA8AEDE3153C145AD43D22A9A3F50A82E2D02116FC4BF0A576C3A98338F63577`.
The full merged regression gate passed in
`local/graphics-merged-build-tests.log`, including configuration/display tests,
camera hooks, index oracles, depth/window-coordinate tests and graphics GPU
readbacks. The actual Ninja compile rules were also checked for absence of
profiling definitions.

Muted run `20261012-053229-714` used that exact executable at 2560x1440,
FXAA, 16x anisotropy, native 2048-square shadows, sharpening 0.15, VSync auto
and a 30 FPS ceiling. The window client remained 1280x720. Captures
`requested_004122.ppm` and `requested_005106.ppm` were visually checked on the
carrier, including movement, tilted camera, opaque nearby barrels/vehicle and
visible soldiers. This is a bounded visual integration check, not a measured
performance comparison or proof that all campaign artifacts are resolved.
The native window closed normally with exit code 0. The complete run log
contains no D3D12 error/corruption or resource/presentation failure; the existing
optimized-clear warnings remain.

The feedback folder includes native/configured, 1080p and 1440p launchers.
The main launcher reads `config/graphics.ini`; the resolution launchers
override only render resolution. Interactive audio is enabled, while the
automated validation remains muted. Settings require restarting the game.

## Automatic-default feature validation

The automatic-default branch uses main's merged SDK `45faa42` without SDK edits.
Its final full build/test log is `local/graphics-auto-final-tests.log`, and the candidate is
`local/candidates/graphics-auto-defaults/HazeRecomp.exe`. Use
`scripts/play.ps1 -ExecutablePath local/candidates/graphics-auto-defaults/HazeRecomp.exe`
from this worktree, or the adjacent `Play-Haze.cmd`, to launch with its
`config/graphics.ini`. The default worktree build executable is also updated.
Candidate SHA-256 after defaulting to borderless: `01E0E7C414DF6A5D388F0021E0F0991CB1615B0915519D2B37EEFDB96C192ED2`.
The borderless default passed the focused configuration tests in
`local/graphics-borderless-default-test.log` and was rebuilt in
`local/graphics-borderless-default-build.log`.

Automatic-resolution native checks:

| Run | Check |
| --- | --- |
| `20261012-053917-975` | Default INI selected primary monitor 2, rendered at 2560x1440 with a 1280x720 client and native 2048-square shadows; title capture inspected. This preceded the hidden-window DPI probe refinement. |
| `20261012-054153-212` | Final candidate, partial INI specifying borderless and monitor 4 only: defaults resolved to 1920x1080, matching both the client and captured title image. |

These bounded runs ended at their 50/45-second limits, with no D3D12
error/corruption or resource/presentation failures. They verify startup and
presentation; they do not measure campaign performance.

## Combined automatic-default feedback build

Main merge `7a7050a` incorporates `10b87ee` and keyboard release fix `fb4980a`,
with SDK `45faa42`. The complete merged gate passed in
`local/auto-defaults-merged-build-tests.log`, including automatic display/INI,
native input, runtime, depth, WPOS, shadow and graphics readback checks.
The build configuration guard verified profiling and comparison were disabled;
SPU FMA was enabled.

Fresh executable: `local/releases/haze-wip-2026-10-12-auto-defaults/HazeRecomp.exe`.
SHA-256: `971EB2469158EB1A5EA70F8D7CAA870ECD447E55CB327559B190F2737C7B523A`.
The ordinary main build executable and PDB were updated to this exact build.
The adjacent launchers use configured/default graphics, or override only the
rendering resolution to 1080p/1440p. Interactive audio is enabled.

Muted native check `local/runs/auto-defaults-merged-no-ini` used this executable
in an isolated working directory with no INI and no explicit graphics path.
The process logged automatic primary-monitor resolution 2560x1440, borderless
mode, native shadows and cap 30. The captured 2560x1440 startup image was
visually inspected. After 25 seconds the owned window closed normally with
exit code 0. This verifies omitted-file startup and presentation, not campaign
performance or resolution of the reported lighting/model/conversation issues.
See [squad slowdown profiling](squad-slowdown-profile.md) for current evidence.
