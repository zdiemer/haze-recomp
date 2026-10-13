# Native mouse ownership and menu input

This work starts from main `f0d1e2b`, in branch `fix/mouse-menus`. It addresses
issues #28, #30, and #43 together. The SDK remains pinned to main's `8660186`.

The window captures automatically once ready and foreground in Auto or
KeyboardMouse mode. Background launch defers capture until activation. Menus
and focus loss temporarily release capture; gameplay restores it. F1 release
persists until another F1 or a gameplay capture click. F1 cannot capture while
a menu owns input. Controller activity releases capture; keyboard activity can
restore it in gameplay unless the user explicitly released it. Controller-only
mode never captures or supplies menu pointer input.

Menus use the Windows pointer. Left click feeds the game's pointer press and
release; right click invokes Back; the wheel supplies list navigation pulses.
Keyboard/controller input returns ownership to the normal guest navigation.
Client coordinates are normalized using the current client dimensions. The
renderer presents to the full client viewport, so this mapping does not depend
on the guest render resolution, desktop position, DPI, or window decorations.

## Guest boundary audit

The local BLUS30094 01.00 executable contains an existing pointer path in its
GUI. The port uses that path so the guest retains its own widget hit testing,
event callbacks, layout, clipping, sliders, toggles, tabs, and menu transitions.
No extracted game source or assets are included in this change.

- Global `F48DD4` is the GUI manager. `0030CEE0` returns its current context at
  `+48`. `0030D398` returns that context's live menu-stack count at `+28C`.
- `0031B9C0` checks the context input pointer at `+170` and a nonempty stack
  before invoking input virtual method `+28`. The bridge publishes ownership
  at this boundary, including when the stack becomes empty.
- The input virtual method `002E0DD8` resets transient input and polls normal
  navigation. The bridge runs afterward. Input `+5C` enables point navigation;
  `+5D` enables directional navigation. Position is `+14/+18`, press origin
  `+1C/+20`, and drag position `+24/+28`. `+4C` is confirm (1 held, 2 released),
  and `+50` is Back. These match the guest pointer producer `002E0518`.
- `002EFC20` chooses the widget at the pointer in point mode, using `00317618`.
  That function searches visible top-level pages by draw order, then descends
  through `003145F0`. Widget bounds come from the same virtual method `+30`
  used by their drawing code. This avoids separate host hit rectangles.
- Scripted Confirm event 1 in `002F69D8` normally rejects point input at
  `002F6DA0`. The guarded patch allows only a native release whose widget
  equals the dispatcher's existing topmost hit (`r18 == r25`, flag `0x200`).
  The guest eligibility check and existing action at `002F74A8` remain in use.
- Slider hit testing in `00301C18` scales the already normalized thumb width
  at `+178` by the widget width a second time. The guarded patch at `00301CFC`
  uses the width stored by drawing routine `00305C80` directly, so the visible
  thumb center accepts a drag. Arrow and vertical hit tests remain native.
- The virtual tab handler `00095EA8` uses base-widget bounds that omit the
  derived drawing code's prompt margin and scrolling offset. Its guarded
  replacement keeps native eligibility (`002EFC20`), uses the drawing layout
  (`00098038`) for each visible tab, and selects through `00305510`. Keyboard
  and controller updates continue through the original handler.
- The camera hook checks live guest menu ownership immediately before writing
  local-player yaw/pitch. `motor+A0` alone only indicates a motor update and
  does not establish gameplay input ownership. The manager clears its temporary
  current-context slot between GUI calls, so the bridge also checks the last
  dispatched context's live stack. Pending motion is consumed and
  discarded when ownership forbids a camera update.

`patch_menu_input.py` validates SHA-256 hashes of four complete translated
functions and patches build-local copies. Direct and virtual callers therefore
use the same boundary. Runtime signatures guard the object layout. The shared
generated translation and SDK checkout are not modified.

## Validation

Automated checks cover the actual port input mapper, camera hook, menu bridge,
capture policy, real SDL virtual gamepads, and prompt atlas behavior. The patch
test checks the local audited translation plus mutation and double-patch
rejection. Tab tests cover native eligibility, drawing bounds, hidden tabs,
selection callbacks, and fallback to normal navigation. A pause request that
the guest ignores during loading releases capture for one second, then permits
recovery; actual menu ownership continues to block capture independently.

Commands run with the pinned SDK and shared generated translation:

```powershell
./scripts/build.ps1 -SharedRoot E:/Code/haze-recomp -SdkRoot E:/Code/haze-recomp/.worktrees/main-integration/external/ps3recomp -SDL3Root E:/Code/haze-recomp/.worktrees/main-integration/build/haze/_deps/sdl3-src -Jobs 1
./scripts/test-input.ps1 -SharedRoot E:/Code/haze-recomp -SdkRoot E:/Code/haze-recomp/.worktrees/main-integration/external/ps3recomp -SDL3Root E:/Code/haze-recomp/.worktrees/main-integration/build/haze/_deps/sdl3-src
python tests/test_menu_input_patch.py E:/Code/haze-recomp/generated/ppu/ppu_recomp_001.cpp
```

All passed. In-game checks used a separate copy of the local save, Windows mouse
events, frame captures, and read-only inspection of guest input/camera state:

- Mouse-only Main Menu → Settings → Sound Setup, slider arrows, thumb drag
  (music 50 → 20), subtitles Off → On, right-click Back, and save confirmation.
- Campaign/checkpoint dialogs, pause-menu Continue, and clicks at the centers
  of both Game Settings and Objectives tabs.
- Three wheel detents moved selection by three rows without stationary hover
  undoing the selection. Keyboard navigation relinquishes pointer ownership.
- Windowed 1280×720, 960×540 output with 1920×1080 rendering, and borderless
  2560×1440 output with 1280×720 rendering retained aligned pointer positions.
- Direct diagnostic launches and `scripts/play.ps1` captured foreground
  gameplay automatically. F1 release persisted, pause exposed the pointer,
  returning to gameplay recaptured, and minimize/restore released/recaptured.
  Launcher sessions closed normally with exit code 0.
- With a paused local-player motor still active, F1 and pointer motion left
  yaw/pitch unchanged; mouse Continue resumed without accumulated motion.
  The same ownership check passed for a menu opened using guest pad Start.
- Controller-only launcher mode kept the cursor visible and unclipped through
  activation, F1, and clicks, with no mouse-capture log entry.
- Fullscreen launch reported DXGI exclusive mode active. After a focus change,
  DXGI left exclusive mode; tab/capture checks then ran in its fullscreen-sized
  window. Continued exclusive-mode interaction across focus changes was not
  established by this run.

No physical controller was attached for these checks. Controller switching,
focus/background launch, and input isolation have production-mapper/SDL virtual
gamepad coverage; the live pad-Start check is not a physical-controller test.
