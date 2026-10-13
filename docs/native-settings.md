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
