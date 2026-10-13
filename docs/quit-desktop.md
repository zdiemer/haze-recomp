# Quit to Desktop: implementation status

Work in progress for #57, based on the shared settings and input infrastructure
in #67. On the Steam integration branch, the main-menu footer is **Quit to
Desktop / Add to Steam** and opens Desktop Options; its first row opens the
normal Quit confirmation. The pause menu still has a direct **Quit to Desktop**
action at the lower right. Click the footer, press the current Select binding (Tab by
default), or press Tab as a keyboard fallback. The controller uses its mapped
Select button, with the current Kenney glyph and a readable binding name beside
the action (for example, **Tab** or Xbox **View**). Rebinding updates that label;
an unbound Select retains the explicit Tab fallback.

The footer declines mouse activation and recognizes its own window as part of
the game's focus group. If it nevertheless receives foreground ownership, a
click hands focus to the game before opening the confirmation or Desktop
Options. Previously that state rejected the click and hid the footer. A
regression reproduces the rejected click before the fix; external-app focus
still prevents opening a dialog.

The confirmation defaults to **Cancel**. Arrows or the current controller menu
directions change the selection; Enter/current Confirm activates it and
Escape/current Back cancels. Losing focus cancels an unconfirmed request.
Gameplay confirmation warns that progress since the last completed checkpoint
may be lost. Returning to the title remains a separate game action. The retail
menu stays underneath; Cancel returns after released input without injecting
another Back command.

## Save coordination

A quit request belongs to a main/pause menu generation. Confirming begins a
wait for observed saves and queued Audio settings work. Cancellation leaves
the save running and returns the coordinator to Idle. A failed or unavailable
save result keeps the game open with an error; elapsed time never means a save
succeeded. Gameplay confirmation text says progress since the last completed
checkpoint may be lost. Quitting does not request a new checkpoint save.

At the audited common guest frame boundary, the bridge checks the live menu
stack, the guest save-busy flag, pending Audio work and save completion. An
atomic admission seal prevents another observed save-worker launch after that
check. The seal requires an unchanged ticket, so even a fast save that starts
and completes between checks must have its result observed. The existing save
launcher hook reports failure before writing its busy flag or creating a
worker if admission is closed. Load workers remain outside this observer.

Once ready, the bridge parks its owning guest thread on a native event at that boundary. The
window/render thread continues. Cancellation releases the seal and resumes the
unchanged guest frame; the host may commit shutdown only after the producer
reports that it is parked. The commit can happen once. The original save
serialization and worker cleanup remain intact.

The bridge recognizes main page `fefl_KS_page2` and pause page `ingm_DC_page1`.
It retains the dispatched GUI context because the guest clears its temporary
current-context slot between calls. It checks the live stack again every frame
so a delayed action cannot quit from a different menu or from resumed gameplay.

## Native cleanup

The window/render thread consumes the shutdown commit once. It releases input
capture and closes PC menus, stops the audio mixer and output backend, waits
for submitted GPU work, relinquishes exclusive fullscreen mode, clears the
display execution-state request, flushes streams and exits with code 0. The
audio-worker and GPU waits each have a two-second bound. Failure is logged and
retains resources that may still be in use until process teardown. Guest audio
port state and its mutex also remain intact for outstanding worker calls;
normal process exit ends those workers. The audio extension compiles the pinned
SDK source once and is guarded by its hash, as with the renderer extension.

The render loop consumes a committed exit before another input/settings update.
Final menu teardown skips provider callbacks and retires the active page before
destroying its window. An unconfirmed Video preview is never saved or rolled
back through the normal rendering wait during this teardown; renderer shutdown
relinquishes exclusive mode and process exit removes the windows. Closing only
the settings menu still reverts its preview. The two-second GPU bound applies
to the final shutdown wait, not a render operation already in progress before
the exit is committed.

After the guest frame loop starts, ordinary window-close requests use the same
save/settings coordination without an extra confirmation. Before that boundary
is available, the existing early-boot window-close behavior remains. Failed
saves keep the process open and offer a return to the game. No new checkpoint
is requested or claimed by quitting.

## Evidence and remaining work

The full input test runner passed in `local/quit-publish-tests.log`, including
actual coordinator/save-observer concurrency, failed and fast saves, stale
menu generations, cancellation while saving, admission recovery and single
shutdown commit. A bounded guest-memory fixture runs the actual bridge with
the generated CPU context. It checks main/pause identity, Audio and guest-busy
waits, thread ownership, producer parking and context preservation on cancel.
Generated-source tests check that rejection precedes launcher writes and that
the frame hook retains the original pacing and save code. The full native build
passed in `local/quit-save-build.log` against SDK `8660186`.

The actual Win32 action/confirmation tests cover keyboard, mouse and controller
access, Cancel as the default, held-input suppression, failure return and focus
cancellation. Main/pause confirmation rendering was captured and inspected in
the harness. Mapper tests verify Quit input suppression and deferred window
close. Backend seams check bounded waits, GPU signal/event/display failures,
audio join failure, and retention of resources still in use.

Native run `local/runs/quit-native-1` reached the main menu, then closed through
the new window-close path. Audio output stopped, the GPU/display step completed,
and the process exited with code 0. Native run `quit-native-2` opened Audio with
Music 70, queued an Apply to 65, then immediately posted window-close. Logs show
the close request preceding save-worker launch, successful profile-save
completion and Audio acknowledgement, then admission sealing, audio/render
cleanup and exit code 0. Profiles were isolated copies; the user's saves were
not modified.

Native run `quit-native-3` restarted with run 2's saved profile and loaded Music
65. With that profile held against writing, an Apply to 60 followed immediately
by window-close reported save failure `0x8002b402`. Audio restored the old
settings, Quit reported the failure, the game stayed open, and the saved file's
hash remained unchanged. After cancelling that failed quit, a new window-close
request performed cleanup normally. This tests write denial before truncation;
it does not establish atomic recovery from a partial write.

Native probes used diagnostic pad input and posted window messages. Windows
refused foreground activation, so these checks do not establish visible native
menu interaction or physical input. Harness foreground focus is injected.
Remaining acceptance includes:

- Verify the action's actual visibility and physical mouse/keyboard/controller
  operation in main and pause menus, including exclusive fullscreen and DPI.
- Verify Cancel preserves focus and resumed gameplay, rebound/unbound Select
  behavior, device switching and displayed prompts in the native game.
- Exercise quitting from pause during a save and after extended gameplay;
  reload the last completed checkpoint and verify launcher/Steam exit status.
- Exercise backend failure/device-removal cleanup in the native game.

The SDK currently writes profile files directly. Waiting for completion is
required for orderly quit, but does not itself make an interrupted or failed
write atomic. That separate persistence gap also remains tracked under #58.

The original PC-only front-end Quit callback sets the guest quit flag, leading
through its CRT exit callbacks. That route was not established as coordinated
host cleanup; the added native action uses the audited frame boundary instead.
