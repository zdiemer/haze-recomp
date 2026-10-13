# Controls settings

Work in progress for #23, stacked on the native settings infrastructure in
#58. **Settings > Control Setup** opens PC Controls. The pause menu references
the same retail page; running that route during gameplay remains an acceptance
check.

The page lists keyboard movement, mouse look, all sixteen emulated controller
buttons, controller movement/look axes, mouse sensitivity and inversion,
controller deadzones and trigger threshold, controller look inversion, prompt
family and an optional SDL mapping file. Action names describe Haze's Default
layout. **Game control options** opens the original page for its controller
layouts, toggle aim/crouch and other game preferences, discarding unapplied PC
edits. Those game preferences can further change the actions or inversion
associated with the emulated buttons and sticks.

## Editing

Select a binding, release held inputs, then press a key, mouse button or
controller input. Review the captured input before staging it. Controller
button rows accept buttons, axis directions and hats; movement/look rows
capture full axes. **Unbind** stages None. Escape or Cancel leaves the old
binding in place. Losing focus cancels capture and its review dialog; losing
the selected controller cancels an active controller capture.

Duplicate sources offer **Keep both**, **Replace others** or **Cancel**.
Replace others unbinds the conflicting actions. Keyboard comparisons ignore
case; controller comparisons recognize equivalent named face-button aliases.
A named SDL source and a manually configured raw source for the same physical
input are not yet resolved as equivalent by the conflict check.

**Apply** validates and saves to the same input.ini selected at startup, then
reloads the actual input mapper immediately. **Back** discards unapplied edits;
**Defaults** stages the shipped defaults and also requires Apply. The writer
preserves unrelated sections, comments and unknown settings, rejects duplicate
managed keys/sections, checks for edits made elsewhere, and writes and flushes a
temporary sibling before replacing the destination. A failed write leaves the
previous settings active. Reopen Controls after an external edit conflict.

The page displays the selected controller's name and explains unrecognized
devices. SDL mapping files use the gamecontrollerdb text format; relative paths
are resolved beside input.ini. A basic format/readability check precedes Apply.
New mappings load immediately, but clearing or replacing a file requires a
restart to discard mappings SDL has already loaded. Raw input bindings remain
available for devices with no SDL gamepad mapping.

Launch options still apply: `HAZE_INPUT=keyboard` disables the controller
backend and capture; `HAZE_INPUT=controller` disables gameplay keyboard/mouse
input. Relaunch in automatic input mode to use both. These overrides do not
change the saved bindings.

## Input and prompts

Keyboard names include A-Z, 0-9, Space, Enter, Escape, Tab, Shift, Ctrl, Alt,
arrows, Mouse1-Mouse5, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
and F2-F12 except F6. F1 controls mouse capture and F6 reloads graphics settings.
Escape is reserved for cancelling a binding capture; existing Escape bindings
in the INI remain supported by the mapper.

Controller names describe physical positions: south/east/west/north, shoulders,
stick presses, menu buttons, d-pad directions and named SDL axes. Raw sources
use zero-based indices: `raw:b0`, `raw:h0.1` (up; masks 2/4/8 mean right/down/left),
`raw:+a0`, `raw:-a0`, or full `raw:a0`. A negative axis source reverses the axis.
Unrecognized raw controls use a generic controller glyph and a numbered label.

Rows preview their binding with the installed Kenney PNGs. Applied bindings and
device activity also update the normal game prompt atlas. Controller style can
be automatic, Xbox, PlayStation or Switch. Mouse sensitivity spans 0.01-10
degrees per device count; movement/look deadzones and trigger threshold span
0-0.94. Mouse look uses raw motion, independently of stick deadzones.

The PC page retains arrow/Enter/Escape navigation. If controller Confirm, Back
or Defaults is unbound, duplicated with another of those actions, or bound to
an axis, the page uses standard face buttons for navigation and shows those
recovery prompts. Unrecognized devices use buttons 1/2/3. Missing movement
axes similarly retain a settings-only left-stick fallback. Gameplay bindings
are unaffected by this fallback. Applying new bindings requires neutral input
before navigation resumes. The host mapper suppresses gameplay input while
the PC page or capture dialog owns it.

## Validation and remaining acceptance

The native build and complete input test runner passed against SDK `8660186`.
Tests exercise the actual settings writer, provider, Win32 procedure, SDL
virtual controllers, host input mapper and Kenney PNG renderer. Coverage
includes keyboard/mouse parsing, controller button/axis/hat capture, neutral
gates, conflict choices, defaults, Apply/reopen, immediate controller remapping,
focus loss, disconnect, raw unmapped devices, prompt families, locked writes,
external edits and preservation of unrelated INI data. The actual Win32 page is
rendered in the harness at 1080p and 4K owner sizes. The existing shared-page
suite also covers other sizes and asynchronous Audio saves.

Native run `local/runs/controls-native-1` opened PC Controls through the game
and changed sensitivity from 0.10 to 0.11. The saved INI and immediate mapper
reload matched. Holding the file against replacement made a subsequent 0.12
Apply fail with its saved hash unchanged. Unrelated prompt/startup settings
were preserved. Native run `controls-native-2` opened **Game control options**,
captured the original Control Setup page with its layout/toggle options, then
reopened PC Controls. Both owned processes exited normally with code 0.

Those native probes used diagnostic pad input and posted window messages;
Windows refused foreground activation. They do not establish physical capture,
visible native-window interaction, gameplay input isolation or immediate
gameplay effects. Harness focus is injected. Remaining checks include physical
keyboard/mouse/controller binding and hotplug, main and pause navigation,
gameplay use after Apply, full-process restart persistence of new bindings,
Alt-Tab/cancel behavior, controller-only recovery and DPI readability. Conflict
detection between raw and named physical aliases also remains incomplete.

Local evidence: `local/controls-release-tests.log`,
`local/controls-release-build.log`, `local/controls-page.bmp`,
`local/controls-capture.bmp`, `local/controls-conflict.bmp`, and
`local/controls-4k.bmp`. Runtime captures and game assets are not committed.
