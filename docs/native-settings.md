# Native settings pages

Work in progress for #58, built on the live graphics backend from #10. This
branch currently provides native **Video** and **Audio** pages and shared
page/input infrastructure. Controls still uses the retail page; its provider
and rebinding belong to the separate #23 PR.

## Navigation and persistence

Open Settings > Video Setup from the main menu. The same retail Video Setup
page is referenced by the pause menu. The port observes activation of that page
and opens a game-owned native window with keyboard, mouse and controller
navigation. The hook preserves the original page activation and page stack;
unsupported game code or pages without a registered provider remain untouched.

Arrow keys and the current mapped menu directions navigate; Enter or the
current Confirm binding selects. Escape or the current Back binding dismisses
an open dropdown first, then leaves the page. The current Defaults binding
stages defaults. Mouse clicks, wheel scrolling and slider dragging are supported.
The footer draws the current keyboard/controller glyphs from the same atlas as
the game. The page releases mouse capture, hides when another app is active,
and requires neutral controller input after opening or returning to focus.

Changes remain staged until **Apply**. Successful application saves through the
existing `graphics.ini` transaction and shows its result. **Back** discards edits
made since the last successful Apply. Defaults also require Apply. Validation,
failed writes and external file conflicts leave the last effective settings in
place. See [live video settings](video-live-settings.md) for the transaction,
override and rollback contracts.

Display mode, monitor and window size now use a 15-second preview after Apply.
The confirmation defaults to **Revert**; **Keep** saves the complete staged
request. Until Keep, the file and quality settings remain unchanged. Escape,
the mapped Back button, closing the confirmation, losing focus, minimizing or
the deadline restore the previous settings. Keep arriving at or after the
deadline cannot save. A failed save also restores the display; a failed
restoration remains visible with a retry action rather than claiming success.
The timer runs in the render loop independently of menu visibility.

Render resolution and shadow resolution still require a restart. Other live changes include frame limit,
VSync, FXAA, texture filtering, mip bias, sharpening and brightness. The page
explains saved values that differ from active launch overrides. Monitor choices
are currently numbered; unavailable displays fail validation on Apply.

While the PC page owns input, the host mapper neutralizes gameplay buttons,
sticks and mouse motion. Back waits for focused neutral physical input before
reporting Circle to the untouched retail page, returning through its normal
navigation. The diagnostic `PAD_FILE` override bypasses that mapper and is not
evidence of gameplay input isolation.

## Validation so far

- Native build against main `f0d1e2b` and its pinned SDK `8660186` succeeded.
- Actual mapper tests cover modal suppression, held-input release and the
  return-to-retail sequence. Page identification tests cover exact IDs, bounds
  and unsupported instructions. The generated-source guard tests preserve the
  original activation lift and reject changed/duplicate function boundaries.
- Win32 tests execute the actual page procedure and Video provider with
  controlled focus/backend results. They cover rebound Confirm/Back/Defaults,
  dropdown selection and scrolling, sliders, Apply/Cancel/reopen, failed writes,
  error recovery, capture release, controller neutral gating and rendering at
  960x540, 1280x720, 1920x1080, 2560x1440, 3440x1440 and 3840x2160 owner sizes.
  These tests do not establish physical-device behavior or DPI acceptance.
- The actual Video provider and Win32 confirmation procedure cover default
  Revert, explicit mouse/keyboard/controller Keep, mapped Back, Escape, window
  close, held controller input, focus loss, timeout result handling, failed save
  and retry after failed restoration. The countdown and buttons were inspected
  in the 960x540 rendering fixture. Backend clock and display boundaries are
  tested separately with real INI writes, including stale/locked files and
  deadline races. Actual GPU fullscreen transitions remain unverified.
- An isolated native 2560x1440 run navigated the retail main menu into Video
  Setup using diagnostic pad input. The guest hook opened the PC provider.
  Posted window messages selected/applied Low, then selected/applied a 60 FPS
  limit. The saved file matched and measured presentation rose from about
  29 FPS to 58 FPS. Applying an unavailable monitor preserved the saved file.
  The process exited normally with code 0.
- That run could not obtain foreground focus. Cross-process window captures
  were black; they are not counted as visual acceptance. The actual page's
  same-process GDI rendering was captured and inspected in the Win32 harness.
  Returning to Settings in the native run used diagnostic Circle because the
  normal return gate correctly waited for foreground focus.

Local evidence is under `local/runs/settings-native-2`, `local/menu-test.log`
and `local/video-page-test.png`. Timed-confirmation evidence is in
`local/video-confirm-menu-tests2.log` and `local/video-display-confirm-540p.png`;
disc assets, runtime captures and generated
game code are not committed.

## Pending saves and Audio integration

The shared provider protocol distinguishes Applied, Pending and Failed. While
an Apply is pending, the page keeps rendering and polls for a result even if
Haze loses focus. It locks further edits, duplicate Apply, Defaults and Back
until that request finishes. A queued save is displayed as Saving, not as a
successful write. Tests cover delayed success/failure and input suppression
during the pending state.

Sound Setup now opens the Audio provider with Music, Sound effects, Dialogue
and Subtitles. It reads current values and defaults from the selected game
profile through the original menu callbacks. Sliders use the original 0–100
range and steps of five. Defaults and edits remain staged until Apply; Back
discards them. The original headset and voice preferences are retained.

Apply queues work to the guest thread that opened the page. A guarded hook at
the common frame boundary consumes it, including in menus that skip the guest
pacing branch. Guest calls preserve the full CPU context and trampoline and
borrow space below that thread's stack, avoiding shared callback scratch space.
The bridge validates the active page/profile and baseline values before writing,
waits for existing saves, invokes the original live-apply/save callback, and
tracks its completion ticket. On failure it restores the previous profile
fields, widgets and live mix. If the profile or edited fields changed elsewhere
during the save, it reports that conflict without overwriting the newer state.
Failed page validation retains the retail page and invalidates the old session.

The port now observes the original save-worker launcher and completion through
guarded build-local patches, composed with the existing pacing patch. It does
not replace serialization, file operations, callbacks or the worker's exit.
Only the audited save-worker descriptor is tracked; load workers are ignored.
Each observed operation receives a ticket so a stale result cannot complete a
later request. A successful thread launch remains pending. A failed launch is
terminal; otherwise the result comes from the guest's processed save-data
return code immediately before worker exit. A worker that skips that result
path reports an unknown failure instead of reusing an earlier success.
Recent ticket results remain available if another save starts before the menu
thread observes completion.

Native validation in `local/runs/audio-save-2` used the retail Sound Setup page
and an isolated copied profile. Changing music and accepting the save produced
ticket 1/result 0, a changed `_PROFILE.DAT`, and the original return to Settings.
A second attempt held that profile file open with exclusive sharing: ticket 2
reported `0x8002b402`, the profile hash remained unchanged, and the process later
closed normally with code 0. This established the save observer before adding
the PC provider.

The PC Audio run in `local/runs/audio-page-4` changed Music from 70 to 50 and
then to zero through posted pointer messages. Both profile saves completed.
The captured mixer output changed from nonzero audio (two-second RMS 0.0234)
to exact silence at zero. Blocking the next profile write with an exclusive
file lock produced `0x8002b402`, restored the prior silent mix, and retained the
saved profile hash. Defaults followed by Back/reopen retained Music zero.
After saving 50, a fresh process using the resulting copied profile opened
Audio with 50 (`local/runs/audio-page-5`). Both processes exited with code 0.
Device output was muted while capturing the actual pre-mute mix; this is a
signal-level check, not a listening test. Effects/dialogue and subtitles still
need gameplay effect checks. Native probes use diagnostic pad input and posted
window messages; the foreground/input limitations above apply to Audio too.

The subsequent native run `local/runs/audio-page-6` enabled Subtitles and saved
successfully. An attempted disable with the profile file locked failed, retained
the saved hash, and reopening Audio still reported Subtitles On. This validates
the original toggle/save/rollback path; displaying dialogue subtitles during
gameplay remains an acceptance check.

The actual bridge/provider tests cover high-address guest heap allocations,
context preservation, defaults, strict staging, subtitle rollback with unrelated
flags preserved, thread ownership, nested calls, pending saves, launch failure,
save failure, interleaved result observation, stale generations, external edits
and unsupported code. The native build and complete test runner passed in
`local/audio-subtitles-build.log` and `local/audio-subtitles-tests.log`.
An earlier suite run hit the existing graphics persistence assertion at
`test_haze_graphics.cpp:102`; the unchanged standalone rerun and two subsequent
full suites passed. That intermittent failure has not been explained.

The current BLUS30094 Audio audit establishes this path:

| Function | Role |
| --- | --- |
| `0x000FFF20` | Finds a named slider, sets its value and marks the widget tree dirty. |
| `0x00100230` | Selects the named toggle's value, used for Subtitles. |
| `0x00100300` | Copies music/effects/speech/headset and related subtitle/voice preferences into the selected profile, applies the live values and requests persistence. |
| `0x004D2F90` | Applies active-profile mix values; music/effects/dialogue use mixer groups 2/0/1. |
| `0x004E9B80` | Starts profile serialization and dispatch; its return alone does not establish a completed disk write. |
| `0x00721F48` | Starts the serialized save worker and reports thread-creation failure. |
| `0x007222B0` | Executes save-data work, processes its result, clears busy state and exits. |

The existing guest exposes separate music/effects/dialogue mixer groups, not a
verified master control. The current native output backend selects the default
device at startup; its diagnostic mute environment variable is cached and does
not provide mute-on-focus-loss. Master volume, device selection and focus mute
therefore need backend work and coordination with device recovery (#49).

The pinned save-data backend writes profile files directly. A reproduction
against its unchanged write function returned an access error after a short
write had replaced a 1 MiB profile with only 512 KiB of new data. Audio Apply
now marks its save-worker ticket and uses a title-owned transaction around the
audited SDK callback sequence. The original SDK checkout remains untouched;
the build generates a guarded copy and rejects changes to its audited hash.

For an existing Audio profile, one complete `_PROFILE.DAT` write is staged in
a private sibling directory, flushed and closed. It replaces the original
only after the final guest save callback succeeds. PARAM.SFO writes are staged
too and must match the existing metadata; Audio does not publish metadata
changes. Missing profiles, unexpected file operations and metadata changes
report failure. Other save/load operations retain the original SDK behavior.

Real-file tests cover short writes, flush/close/replacement failures, a failed
final callback, duplicate writes, metadata failures, profile locking, competing
Audio transactions, external edits, cleanup and successful publication. The
Audio bridge tests verify that only its own Apply arms the transaction.
Two native runs of combined build `1c52af1` then exercised this hook with the
original guest callbacks and a copied profile. In `audio-transaction-native-2`,
Audio Apply changed Music 65 to 60 and committed successfully. A subsequent
55 request held the profile open with read sharing but without delete sharing:
the SDK completed its 1 MiB file operation and final callback, then the final
replacement failed with `0x8002b402`. The page reported rollback; the profile
and metadata hashes stayed unchanged and the staging directory was removed.
The process remained alive and later exited normally with code 0.

A fresh process in `audio-transaction-native-3` loaded that saved profile and
opened Audio with Music 60. It also exited 0 without changing the saved hash
(`73f8419d356b2195978e76142eb4961dba45bae88ab3c23d4399e5fcd3ae4142`).
Logs and reports are under the combined worktree's `local/runs/`. These runs
used explicit Win32 desktop assignment to a nonvisible desktop, diagnostic
pad input and posted menu keys; the visible input desktop stayed unchanged.
They establish native callback/persistence behavior, not listening, physical
input, visual focus behavior or fullscreen acceptance.

This is not a general checkpoint/new-profile recovery system: it does not coordinate nonparticipating
external writers after the last conflict check or guarantee recovery from
power loss. Broader save recovery belongs to #48.

## Work still required before #58 can close

- Finish Audio gameplay effect and listening checks. The audit above identifies
  unsupported master/device/focus-mute controls as separate backend work;
  device recovery remains coordinated with #49.
- Validate the separate Controls provider from #23 in the combined main/pause
  flow; its implementation and automated tests are integrated in the candidate.
- Finish native display checks for exclusive fullscreen, focus loss, monitor
  changes and restart persistence. Window-size timeout and save-on-Keep passed
  in the combined candidate's `video-native-1` run (see video-live-settings.md).
  The automated UI/DXGI seams do not establish that the owned popup remains
  visible or that exclusive entry succeeds while the settings window has focus.
- Verify physical keyboard/mouse/controller interaction in both main and pause
  menus, focus/Alt-Tab behavior, gameplay input isolation, DPI/readability,
  failed persistence and restart persistence in the full game.
- Correct and retest Back navigation: in `video-native-1`, Escape closed the
  native Controls panel but left the retail Control Setup page underneath.
  An explicit guest Circle pulse then returned normally. The previous synthetic
  Back lasted only two backend polls; the SDK polls at 4 ms intervals from both
  data and information calls, so those polls do not prove guest consumption.
- Verify repeated graphics apply/reset under sustained gameplay and finish the
  GPU acceptance checks in #10. Shared retail page IDs prove the intended pause
  route, but do not replace running that route.

Quit to Desktop (#57) and Add to Steam (#47) remain separate features/PRs.

## Current-main integration and Back delivery (2026-10-13)

The settings branch now includes main efc7db9 and its SDK revision
3cd8e810caade4907e583bd38febfc684dd472cf. The retail mouse-menu bridge lives in
`haze_menu_pointer.cpp`; the native settings provider remains in `haze_menu.cpp`.
Their guarded generated-code patches are chained, and composition is tested.

Back no longer expires after two host pad polls. Pending returns suppress host
input until the guest GUI consumes its input packet. That consumer waits for
focus, a current controller sample, and released keys/buttons before delivering
one Back event through the existing retail pointer bridge. Queued pointer events
are discarded while the native page owns input. Leaving the guest menu stack
cancels a pending return.

The complete input/menu/audio regression suite passes against the current SDK,
including repeated host polls before guest consumption, focus/sample gating,
single Back delivery, and cancellation after the guest leaves its menus.
The full native FMA build also passes (`local/back-consumption-build.log`), with
executable SHA256 `bfe9be237fb77e2f3bfc2106313b2c07474b38cadd5a29b3a3072f755ee5dd2e`.
Native Back/reopen verification remains pending: a visible test was deferred
because a separate user-owned game was running. The earlier reproduced failure
must not be considered resolved by these tests alone.
