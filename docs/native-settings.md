# Native settings pages

Work in progress for #58, built on the live graphics backend from #10. This
branch currently provides the native **Video** page and shared page/input
infrastructure. Audio and Controls still use the retail pages until their PC
providers are registered. Controls/rebinding belongs to the separate #23 PR.

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

Display mode, monitor, window size, render resolution and shadow resolution are
currently marked as requiring a restart. Live changes include frame limit,
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
and `local/video-page-test.png`; disc assets, runtime captures and generated
game code are not committed.

## Pending saves and Audio integration

The shared provider protocol distinguishes Applied, Pending and Failed. While
an Apply is pending, the page keeps rendering and polls for a result even if
Haze loses focus. It locks further edits, duplicate Apply, Defaults and Back
until that request finishes. A queued save is displayed as Saving, not as a
successful write. Tests cover delayed success/failure and input suppression
during the pending state. The Audio provider itself is not registered yet.

The port now observes the original save-worker launcher and completion through
guarded build-local patches, composed with the existing pacing patch. It does
not replace serialization, file operations, callbacks or the worker's exit.
Only the audited save-worker descriptor is tracked; load workers are ignored.
Each observed operation receives a ticket so a stale result cannot complete a
later request. A successful thread launch remains pending. A failed launch is
terminal; otherwise the result comes from the guest's processed save-data
return code immediately before worker exit. A worker that skips that result
path reports an unknown failure instead of reusing an earlier success.

Native validation in `local/runs/audio-save-2` used the retail Sound Setup page
and an isolated copied profile. Changing music and accepting the save produced
ticket 1/result 0, a changed `_PROFILE.DAT`, and the original return to Settings.
A second attempt held that profile file open with exclusive sharing: ticket 2
reported `0x8002b402`, the profile hash remained unchanged, and the process later
closed normally with code 0. This validates observation of real success/failure;
it does not yet validate a PC Audio provider, audible output or restart recovery.
The native build and full test runner passed (`local/audio-prep-build.log` and
`local/audio-prep-tests.log`).

The current BLUS30094 Audio audit establishes this path:

| Function | Role |
| --- | --- |
| `0x000FFF20` | Finds a named slider, sets its value and marks the widget tree dirty. |
| `0x00100300` | Copies music/effects/speech/headset and related subtitle/voice preferences into the selected profile, applies the live values and requests persistence. |
| `0x004D2F90` | Applies active-profile mix values; music/effects/dialogue use mixer groups 2/0/1. |
| `0x004E9B80` | Starts profile serialization and dispatch; its return alone does not establish a completed disk write. |
| `0x00721F48` | Starts the serialized save worker and reports thread-creation failure. |
| `0x007222B0` | Executes save-data work, processes its result, clears busy state and exits. |

Audio Apply must run through the guest's menu-owning thread, preserve the other
profile preferences, correlate its request with the observed save ticket, and
restore its prior live values if saving fails. Those connections and the native
failure/restart acceptance checks remain to be implemented. The observer alone
does not provide atomic disk rollback, master volume, device selection, or
mute-on-focus-loss.

## Work still required before #58 can close

- Integrate working Audio controls through the game's existing profile/save
  callbacks. Asset/callback audit identifies music, effects and speech sliders;
  a working master control and output-device/focus-mute backend are not yet
  established. Coordinate device recovery with #49.
- Integrate the separate Controls provider from #23.
- Implement and verify timed keep/revert for display changes. A restart marker
  alone does not satisfy this requirement.
- Verify physical keyboard/mouse/controller interaction in both main and pause
  menus, focus/Alt-Tab behavior, gameplay input isolation, DPI/readability,
  failed persistence and restart persistence in the full game.
- Verify repeated graphics apply/reset under sustained gameplay and finish the
  GPU acceptance checks in #10. Shared retail page IDs prove the intended pause
  route, but do not replace running that route.

Quit to Desktop (#57) and Add to Steam (#47) remain separate features/PRs.
