# Add to Steam: implementation and remaining acceptance

Work in progress for #47, stacked on the native menu and Quit work in #68.
The main-menu footer is **Quit to Desktop / Add to Steam**. Click it or use the
current Select binding (Tab by default; Tab remains a keyboard fallback) to
open Desktop Options. The pause-menu footer still opens Quit confirmation
directly. Returning from Desktop Options releases its input ownership without
sending another Back command to the retail main menu.

Desktop Options contains Quit to Desktop, a Steam profile selector, launcher
information and an artwork description. **Add to Steam** reviews the selected
account and executable before writing. The confirmation defaults to Cancel;
Escape, the current Back binding, window close and loss of focus cancel it.
No profile is selected automatically, including when only one exists. Refresh
rediscovers Steam and clears the selection. Missing Steam or a missing launcher
produces an actionable message and does not affect normal play.

## Launcher dependency

The target is `HazeLauncher.exe` beside the native game, or an absolute path in
`HAZE_LAUNCHER_PATH`. The shortcut quotes that executable and its parent working
directory. When the launcher exports `HAZE_LAUNCHER_ARGUMENTS`, those canonical
arguments retain the resolved game/save/settings/cache paths and runtime mode,
including paths selected through command-line or environment overrides. Without
that export, the shortcut uses the launcher's saved defaults. The launcher must resolve its saved
game-data/settings/cache configuration independently of Steam's working
directory. Its embedded icon is also the shortcut icon. This is the integration
contract for #42 and #41; **those dependencies are still open, and this branch
does not yet supply their executable or embedded icon**. It refuses to register
a nonexistent launcher. A file with the expected name is not proof of a correct
launcher; actual Steam launch acceptance remains required.

The local `PS3_VFS_ROOT/PS3_GAME` folder supplies artwork. No artwork is fetched
from the network or checked into Git.

## Shortcut registration

Steam installation discovery uses its current-user registry key, then the
32-bit and 64-bit machine install keys. Profiles are existing nonzero numeric
`userdata/<account-id>/config` directories. The menu currently identifies them
by account ID; friendly account names are a remaining usability improvement.
The chosen root/account/config relationship is revalidated at write time.
Redirected paths (junctions/symlinks) are refused with an error rather than
silently writing through a different account location.

The binary KeyValues reader is bounded by file size, nesting, field count and
string length. It preserves the order, casing, data and unknown supported
fields of unrelated entries. Unsupported types, truncation, duplicate keys or
malformed structure fail without replacing the file. An existing empty file is
treated as corruption; a missing file can be created. Duplicate IDs belonging
only to unrelated entries are retained. A collision involving the new or
managed Haze ID is refused.

New entries use a deterministic high-bit 32-bit ID and the category
`Haze Recomp (managed)` as their persistent ownership marker. Repeated activation
updates that one entry, retaining its ID, display name, history, categories,
unknown fields and custom icon. The default executable icon follows an install
move. Exe, StartDir and LaunchOptions follow the current launcher contract.
Do not remove the managed category if you want automatic relocation repair.
An unmarked Haze-named or same-executable shortcut is left alone and reported
instead of silently adopting it or adding a duplicate.

Steam and its web helper must be closed. The operation checks running processes,
holds an exclusive read handle on the selected `steam.exe` to deny a competing
client start, checks processes again, and holds an account update lock. It never
terminates Steam, a game, or the service. An inaccessible process snapshot or
busy executable aborts the operation. This loader exclusion was exercised with
an owned child executable named `steam.exe`, not inferred from a process check.

An existing shortcuts file is read while denying writers. The replacement is
written to a unique sibling and flushed; an independently flushed, uniquely
named `shortcuts.vdf.haze-backup-*` preserves the old bytes. The file name is
re-read before commit to reject changes made during preparation. Its read
handle is then closed for Windows replacement, while the client/account locks
remain held. A sibling `MoveFileExW` rename commits the replacement. New-file
installation does not allow replacement. Failed/short writes clean up only the
temporary files this operation created. A failed replacement retains the old
file and completed recovery backup. Backups are never rotated or overwritten.

The executable gate protects against the selected Steam client, and the account
lock serializes Haze writers. This is not a filesystem transaction against
arbitrary third-party editors or another Steam binary deliberately pointed at
the same profile. Do not edit the same Steam configuration concurrently with
another library manager. Filesystem crash/power-loss durability has not been
verified beyond Windows flush/write-through semantics.

## Local artwork

Images are generated with Windows Imaging Component. Scaling is uniform,
transparent pixels are composited correctly, and surrounding canvas provides
padding instead of stretching/cropping the source composition:

| Steam role | Local source | Output |
| --- | --- | --- |
| Portrait cover | ICON0.PNG logo + PIC2.PNG character | 600 × 900 |
| Landscape library tile | PIC0.PNG | 920 × 430 |
| Hero/banner | PIC1.PNG | 1920 × 620 |
| Logo | ICON0.PNG, transparent margin removed | Original visible logo size |

The supplied character is only 310 × 250 and the visible logo is 213 × 58, so
the portrait cover is visibly softer than original high-resolution cover art.
The hero is padded because the disc image is 1920 × 1080. These are usable local
fallbacks, not reconstructed retail PC artwork. Generated samples were inspected
at `local/steam-art-preview/grid/`.

Files use the stored 32-bit shortcut ID under the selected account's `grid`
folder. Existing matching artwork with any extension, including legacy 64-bit
ID names, takes precedence. Each new PNG is staged, encoded, flushed and renamed
without replacement. Retry fills missing images while retaining all existing
customization. An artwork failure does not roll back a valid shortcut; the
result explicitly reports incomplete artwork and can be retried. The client
and account locks remain held throughout artwork installation.

## Validation

Run `scripts/test-steam.ps1 -SdkRoot <pinned-sdk>` for the model, Windows file,
WIC and actual Win32 menu checks. These use isolated per-process folders and do
not register anything in the user's real Steam library.

Covered: Unicode/spaced paths, malformed/truncated input, unknown-field and
unrelated-entry preservation, duplicate/collision handling, repeated activation,
relocation and customization preservation; explicit profiles and mismatched
account paths; missing launcher; running-client deferral and competing-loader
exclusion; locked files, short staged/backup writes, failed rename and cleanup;
artwork dimensions, alpha, proportions, custom/legacy preservation and missing
sources; keyboard/mouse/controller menu events, held-input gating, Cancel-first
confirmation, Escape/Back/window-close/focus cancellation, stale menu refusal,
and main/pause Quit routing.

The read-only live audit found two profiles and one 13-byte empty shortcuts
list. It round-tripped exactly but provides no populated-library coverage.
Seven public Heroic fixtures at revision
`fc249e021e5d988732f4b05e338fcc972f428f05` supplied that coverage: all nine existing
entries were preserved byte for byte while adding one Haze entry in memory.
Fixtures remain under ignored `local/steam-reference`, not in this repository.

The Win32 menu test renders the actual page and confirmation with an accented
launcher filename. Focus and glyph drawing are injected in that harness; it is
not a physical-device or exclusive-fullscreen acceptance result.

Still required before closing #47: executable launcher/icon dependencies;
friendly profile identification; foreground main-menu access with physical
keyboard/mouse/controller (including rebound/unbound Select), DPI and exclusive
fullscreen; real-client registration/restart persistence and artwork display;
Steam desktop and Big Picture launch with correct saves/settings/cache and
normal exit; and actual install relocation through the launcher.

## Format references

- [Valve's KeyValues reader/writer](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/tier1/KeyValues.cpp)
  informed the supported binary type framing; Steam shortcut-specific behavior
  still requires client acceptance.
- [Heroic shortcut implementation](https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher/blob/fc249e021e5d988732f4b05e338fcc972f428f05/src/backend/shortcuts/nonesteamgame/nonesteamgame.ts)
  and [artwork/ID helper](https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher/blob/fc249e021e5d988732f4b05e338fcc972f428f05/src/backend/shortcuts/nonesteamgame/steamhelper.ts)
  provide a primary implementation comparison. This branch does not enable
  Steam remote debugging or use an undocumented in-process client API.
- [Valve's manual non-Steam instructions](https://help.steampowered.com/en/faqs/view/4B8B-9697-2338-40EC)
  and [library artwork overview](https://store.steampowered.com/libraryupdate/?l=english)
  describe the user-facing client features.
