# Device-aware title prompt

The title screen displays `Press [current Start glyph] to begin`. It uses the
game's existing text component, capitalization, yellow color, position and scale.
The glyph comes from the same live input atlas as the other native prompts:
`Bindings.Start` for keyboard and `ControllerBindings.Start` for the active
controller family. Editing `config/input.ini` updates the atlas while the title
is open. Xbox, PlayStation and Switch labels follow the existing last-active
device behavior and `Controller.IconFamily` override.

The change does not intercept activation. The existing input mapper sends the
configured key/button as guest Start, and the original title event advances the
screen. With prompts disabled, the inline glyph falls back to the retail Start
icon, consistent with the other game prompts.

The menu bridge identifies the topmost `fefl_DC_page3` (Press Start) page and
preserves the configured Start key there. Other menus and dialogs map Enter to
Confirm. A held Enter must be released when crossing that boundary, so starting
the game cannot also confirm the next page.

## Resource boundary

For BLUS30094, string ID `STR_PRESS_START` hashes to `0xb3a1b9c6`.
`STR_CONTROLLERDESCRIPTION_START` (`0xfaf5863e`) confirms the inline Start markup
is `{E}`. The localized resource contains a sorted hash table, a parallel u16
row-index table, and five relative text offsets per row. The guest loader converts
those offsets before looking up display column 4.

`patch_title_prompt.py` inserts one preparation call at `0x005451a0`, before the
original lookup returns the display text for compilation into layout objects.
It patches only a build-local copy of the lift and checks the complete original
function digest. The runtime checks the original nine guest instructions, the
requested string ID, row/column bounds, offset arithmetic, termination, and the
Start markup resource before writing. It does not retain resource pointers.

The English replacement fits the original allocation exactly. Other supplied
translations retain their wording, replacing only `START` with `{E}`. Unknown
wording, different markup or unsupported code is left untouched. No extracted
game asset or generated translation is checked in.

## Validation

Run `scripts/test-input.ps1` with the usual shared/SDK/SDL roots. It covers:

- Title string identity, unrelated strings/columns, localization, bounds,
  unsupported instructions, and repeated lookups.
- A guarded patch that preserves all original lifted code and rejects changed
  or already patched functions.
- Start-slot atlas changes for keyboard rebind/unbind and all three controller
  families; SDL virtual-controller activation after rebinding Start to north.
- Keyboard configuration reload changing the key that emits guest Start.

Native validation on the pinned SDK includes the full build and 2560x1440 and
3840x2160 title captures showing Enter, followed by a live change to J without
restarting. Both processes closed normally with exit code 0. Physical-controller and foreground
keyboard title activation still require an interactive acceptance pass; virtual
input tests do not establish those end-to-end checks.

## Current-main integration (2026-10-13)

Merged main efc7db9, including the retail mouse-menu bridge and audio fixes. The complete input regression suite and full native FMA build pass against main's exact SDK revision 3cd8e810caade4907e583bd38febfc684dd472cf, isolated in menu-sdk-main. The older 8660186 SDK lacks the native-float entry point now required by main. Logs: local/main-sdk-tests.log and local/main-sdk-build.log. Earlier native acceptance evidence predates this integration; remaining interactive checks above are still pending.
