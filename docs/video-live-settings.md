# Live video settings

Press **F6** to explicitly reload the current `graphics.ini`. The window title
shows the result; the log lists every effective value and any launch overrides.
Editing the file alone does not apply it. Reload never writes the file.

This backend is intended for the Video page in #58. That page will submit a
staged configuration through the same transaction, with `persist=true` for its
Apply action. Menu navigation and display keep/revert UI are still separate work;
F6 is the current explicit reload mechanism.

| Settings | Application |
| --- | --- |
| VSync, frame limit | Next presentation; the guest pacing selection follows the cap |
| Anisotropic filtering, mip bias | GPU fence, then invalidate the sampler cache and descriptor tables |
| FXAA, sharpening, exposure | Allocate a private presentation pass if needed, then commit after a GPU fence |
| Display mode, monitor, window size, render resolution, shadow resolution | Saved/requested value reported separately; restart required |

Display and render-target dimensions stay fixed during live application. There
is no live display transition to confirm in this backend. Unknown keys, invalid
values, unavailable monitors, allocation failures and failed writes reject the
transaction. Original launch overrides remain in effect; values the program
wrote to its environment during startup do not become new overrides.

## Presets and defaults

The shared model supplies Low, Medium, High, and detected Custom states to the
Video page. Presets change only these five quality settings, preserving display,
frame cap, VSync, and brightness preferences. Shadow changes require restart.

| Setting | Low | Medium | High |
| --- | --- | --- | --- |
| Anti-aliasing | off | FXAA | FXAA |
| Anisotropic filtering | 1 | 4 | 16 |
| Shadow resolution | 1024 | native (2048) | 4096 |
| Mip bias | 0 | 0 | 0 |
| Sharpening | 0 | 0 | 0.15 |

Custom is derived from actual quality values, including manual INI edits; no
potentially stale preset label is saved. Numerically equivalent float spellings
remain in the same preset. Reset uses the shipped defaults: automatic desktop
resolution, borderless primary display, native shadows, effects off, filtering
1, bias/exposure 0, automatic VSync and a 30 FPS cap.

## Transaction and integration

`haze_graphics_state()` returns configured/effective/overridden values and keys
that still require restart. Build a staged copy of `configured`; use
`haze_video::preset` or `haze_video::defaults` for preset/reset actions. Cancel
discards that copy. Call `haze_graphics_apply` on the window/render thread,
outside renderer calls. Error text is suitable for the settings page.

Preparation allocates into a temporary resource and finishes outstanding GPU
commands before changing descriptor identities. Effective state is untouched
until commit. If saving is requested, a same-directory temporary file is fully
written and flushed, checked against the loaded source for intervening edits,
then atomically replaces the destination. Failure cancels preparation and frees
only temporary resources. The old file and effective settings survive. Explicit
saves serialize the graphics section canonically; comments are not preserved.
The external-edit check detects changes before replacement; it cannot lock an
uncooperative editor out of the final check/replace interval.

The reversible display primitive for the upcoming timed confirmation is now
available as `haze_display_begin` / `haze_display_finish`. It accepts explicit
window mode, monitor and resolved window dimensions without modifying startup
environment variables. It captures window placement, styles, visibility and
fullscreen policy; a rejected transition restores them. Keep discards the
snapshot, while revert restores it. Only one transition may be pending, and a
failed restoration remains observable and cannot be accepted with Keep.

DXGI transitions finish outstanding GPU work and refresh only the scanout
buffers, leaving internal rendering dimensions intact. Denied fullscreen
requests retain existing buffers. Resize or buffer-acquisition failures attempt
to restore the previous mode and scanout resources. An unrecoverable failure
releases exclusive ownership and stops renderer use rather than presenting
through missing buffers. Reverting while unfocused restores the previous
fullscreen preference with exclusive reacquisition deferred to the renderer's
existing focus policy. Window placement restoration preserves hidden/maximized
states and lets Windows recover placement when the original monitor is absent.

`haze_graphics_preview(window, configured, persist, error)` now wraps that
primitive in a 15-second transaction. During preview, configured values and
the file retain their previous contents; effective display values report the
preview. Quality changes wait until Keep so reverting does not need to recreate
old effects resources. `haze_graphics_confirm(revision, keep, error)` rejects
stale confirmations, checks the deadline and current display, and prepares
quality resources and saves only after Keep. A failed preparation or save
restores the previous display; a failed restoration stays visible and retryable.

The render loop's `haze_graphics_update` handles timeout, focus loss and minimize
independently of menu visibility. Status snapshots include a preview revision,
remaining seconds and restoration failure. Graphics status reads remain safe
from synchronous window-message callbacks; an operation guard rejects reentrant
writes without holding the status mutex through Win32 or DXGI calls.

The Video page in this settings branch calls the preview API and supplies a
Revert-first Keep/Revert dialog. F6 continues to use the original reload API,
with display changes reported as requiring restart. F6 is refused during a
pending preview and never writes the file.

The title compiles the pinned SDK renderer once through a small owning-translation-
unit extension. CMake checks its normalized source hash so changed internals
require an audit. The SDK checkout is not modified. Renderer-specific calls run
on its owning thread; the menu never owns descriptors or GPU resources. Frame-cap
changes reach guest pacing through an atomic value; diagnostic pacing overrides
retain precedence.

## Validation and remaining checks

`scripts/test-input.ps1` includes startup parsing and transactional graphics
tests. It exercises repeated preset/reset/save/read cycles, Custom detection,
partial-file defaults, invalid requests, launch overrides, external edits,
locked files, simulated resource-allocation/fence/device failures, cancellation,
sampler invalidation, resource lifetime, and F6 repeat/input isolation.

Native validation uses the full pinned-SDK build and a 1440p title session with
D3D diagnostics: enable FXAA/16x filtering/sharpening/exposure and a 60 FPS cap,
reject an invalid sharpening value, then restore effects off and 30 FPS.
Observed presentation changed from about 29 to 56 FPS and back. No D3D error or
corruption messages occurred. The session also emitted clear-value and unused
render-target warnings already present before application.

Gameplay sampling/shadow behavior, sustained repeated real-GPU applications,
and the #58 menu/controller/persistence acceptance pass remain outstanding.
Failure injection proves the transaction's resource/state ordering; it is not
a real GPU out-of-memory test. Browser rendering is not covered by this backend.

The display primitive additionally has real Win32 window tests for repeated
windowed/borderless previews, exact geometry/style/visibility restoration,
maximized restoration, Keep, overlap and thread refusal, and failed-revert retry.
Those window tests inject the DXGI boundary. The actual renderer extension has
deterministic seams for exclusive transitions, denied modes, partial buffer
acquisition, resize failure, device failure and unsuccessful recovery. The full
native build passes. Native window-size confirmation is covered below;
exclusive-fullscreen acceptance remains incomplete. Implementation follows Microsoft's [fullscreen transition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate)
and [swap-chain resize](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)
requirements.

Settings-transaction tests inject the display boundary and clock while exercising
real INI persistence. They cover the 15-second boundary, Keep exactly at expiry,
focus loss, stale revisions, reentrant readers/writers, wrong-thread confirmation,
launch overrides, quality-only application, locked/stale files, allocation
failure, partial transition failure and failed-revert retry. The preview never
writes the file; successful Keep is verified against the serialized values.

### Native window-size confirmation

The combined candidate (`1c52af1` binary, SDK `8660186`) exercised the actual
Video provider and D3D12 backend in `menu-acceptance/local/runs/video-native-1`.
Starting at 1280x720, a 1920x1080 preview changed the real client geometry while
the graphics INI hash stayed unchanged. The visible Revert-first dialog counted
down; timeout restored the original window rectangle and file hash. Repeating
the preview and choosing Keep retained 1920x1080 and wrote that value to the
INI. The result dialog reported successful application. These were posted-key
checks with the native panel in the foreground, not physical-controller tests.

Fullscreen attempts reached monitor-sized geometry and restored the saved
windowed rectangle without changing its INI. They do not establish usable
exclusive-fullscreen confirmation: the transition changed focus, and a later
Keep attempt lost foreground focus and did not save fullscreen. Exclusive
state, popup visibility, monitor changes and restart persistence still need
verification. The probe exited normally with code 0 and did not alter its
copied save profile.

## Current-main integration (2026-10-13)

Main 4e498ec pins SDK e6b28a0. Its renderer changes add complete-mip upload
validation and an immutable texture-content cache. The cache retains a separate
COM reference and compares format, mip layout and complete bytes before reuse;
sampler settings are outside that identity. Live anisotropy/mip-bias changes
therefore still invalidate only sampler descriptors after the existing GPU
fence. Postprocess allocation, display buffers, frame pacing and their rollback
paths are unchanged. Shutdown clears the new cache after the normal flush (or
releases its references when the renderer has already stopped). The renderer
source guard has been updated after this audit; it still rejects other revisions.

Merged main efc7db9, including the retail mouse-menu bridge and audio fixes. The complete input regression suite and full native FMA build pass against main's exact SDK revision 3cd8e810caade4907e583bd38febfc684dd472cf, isolated in menu-sdk-main. The older 8660186 SDK lacks the native-float entry point now required by main. Logs: local/main-sdk-tests.log and local/main-sdk-build.log. Earlier native acceptance evidence predates this integration; remaining interactive checks above are still pending.

