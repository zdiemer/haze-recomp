# Quit to Desktop: implementation status

Work in progress for #57, based on the shared settings and input infrastructure
in #67. The current change implements save coordination and the guest boundary;
it does **not yet expose a Quit to Desktop menu action or change window-close
behavior**. Menu UI, host cleanup and native acceptance remain required.

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

Once ready, the bridge parks its owning guest thread at that boundary. The
window/render thread continues. Cancellation releases the seal and resumes the
unchanged guest frame; the host may commit shutdown only after the producer
reports that it is parked. The commit can happen once. The original save
serialization and worker cleanup remain intact.

The bridge recognizes main page `fefl_KS_page2` and pause page `ingm_DC_page1`.
It retains the dispatched GUI context because the guest clears its temporary
current-context slot between calls. It checks the live stack again every frame
so a delayed action cannot quit from a different menu or from resumed gameplay.

## Evidence and remaining work

The full input test runner passed in `local/quit-bridge-tests.log`, including
actual coordinator/save-observer concurrency, failed and fast saves, stale
menu generations, cancellation while saving, admission recovery and single
shutdown commit. A bounded guest-memory fixture runs the actual bridge with
the generated CPU context. It checks main/pause identity, Audio and guest-busy
waits, thread ownership, producer parking and context preservation on cancel.
Generated-source tests check that rejection precedes launcher writes and that
the frame hook retains the original pacing and save code. The full native build
passed in `local/quit-bridge-build.log` against SDK `8660186`.

These checks do not prove native menu visibility, physical input, disk safety
during interruption, audio/display cleanup or full-process exit. The remaining
implementation and acceptance are:

- Add a visible Quit to Desktop action to both main and pause menus, reachable
  with mouse, keyboard and controller and displaying the appropriate prompts.
- Keep the gameplay checkpoint-loss confirmation distinct from returning to
  the title; Cancel must preserve menu focus and resume behavior.
- Consume the shutdown commit on the window/render thread, release mouse
  capture, stop output/workers, restore display state and flush logs, with
  bounded backend waits and a normal launcher/Steam exit.
- Retain ordinary window-close behavior while coordinating active writes.
- Exercise both menus, all input methods, active saves, changed settings and
  an extended gameplay session in the native game; verify the last valid
  checkpoint remains readable and no host process remains.

The SDK currently writes profile files directly. Waiting for completion is
required for orderly quit, but does not itself make an interrupted or failed
write atomic. That separate persistence gap also remains tracked under #58.

The original PC-only front-end Quit callback sets the guest quit flag, leading
through its CRT exit callbacks. The audit does not establish coordinated host
audio/render cleanup on that route. The current ordinary window-close path
calls input shutdown and exits immediately. Neither path is counted as #57
acceptance.
