# Local Windows program icon

Windows builds convert `game/disc/PS3_GAME/ICON0.PNG` under the shared asset root
into an ICO containing 16, 24, 32, 48, 64, 128 and 256 pixel images. The complete
source image, including its transparent margins, is centered on a transparent
square without stretching or cropping. The supplied icon is a wide Haze
wordmark; its text is necessarily small at taskbar/list sizes.

Both HazeRecomp.exe and HazeLauncher.exe embed the generated resource. Explorer
and Steam shortcuts can use the executable directly as their icon source; no
separate ICO has to be installed. The game sets both window icon sizes when it
attaches native input and refreshes them on a DPI change. Launcher error dialogs
use the same window artwork while retaining the standard error indicator.

`scripts/build.ps1` and `scripts/build-launcher.ps1` generate the resource
automatically through CMake. `-SharedRoot` selects the existing game data in a
worktree build. Direct CMake builds can set `HAZE_SHARED_ROOT`. Conversion uses
Windows PowerShell and System.Drawing, with no downloaded tools or image assets.
The source PNG and generated ICO/RC files stay in ignored game/build directories.

If the PNG is missing, corrupt, not PNG, over 8 MiB or over 4096 pixels on either
side, conversion warns and falls back to the bundled Haze logo described below;
if that is also unavailable it embeds the standard Windows application icon. A
later build picks up added, changed or removed artwork without reconfiguring.
Unchanged output keeps its timestamp to avoid recompiling the resource. File
write/replacement failures stop the build instead of silently retaining stale
artwork. Generated files are written and flushed before replacement.

## Bundled Haze logo

`assets/haze-logo.png` is a crop of the Haze wordmark from the official 2007
Haze wallpaper distributed through the TimeSplitters World downloads page
(timesplitters-world.de, "Haze Logo" wallpaper). Haze is a trademark of Ubisoft
Entertainment and Free Radical Design; the artwork is bundled solely to
identify this port of the game. `assets/haze-logo.ico` is the same artwork as a
seven-layer ICO in the format `scripts/build-icon.ps1` produces.

The bundled logo covers the builds that have no game artwork. Hosted release
builds use private build inputs containing generated sources only, so no
`ICON0.PNG` exists there: `port/windows-icon.cmake` passes the logo to
`scripts/build-icon.ps1` as `-FallbackSource`, and the game and launcher embed
it instead of the generic Windows application icon. Local builds with game
data still prefer the disc's own `ICON0.PNG`. The setup executable is
downloaded before any disc image exists, so `scripts/installer.py` always
brands it with the bundled ICO (`csc /win32icon`), and the installer window
and taskbar button use the same icon through the form's associated icon.

Run `scripts/test-icon.ps1 -SharedRoot <asset root>` to check ICO layer dimensions,
alpha, aspect ratio, stable output timestamps, missing/corrupt fallback, actual
Windows resource decoding, and large/small window icon installation. Its
`build/icon-tests/HazeIconTests.exe` also accepts executable paths to inspect
their embedded resources without running them; `--icon-file <ICO>` verifies
the exact embedded image bytes against that file. A separate clean fixture
build checks missing artwork, then adds and removes artwork without CMake
reconfiguration and verifies the executable resource after each build.
The fixture window remains
hidden; it does not establish visual Explorer, shortcut, taskbar, Alt+Tab or
multi-monitor DPI acceptance, which require an interactive desktop check.

Validation in this worktree: `local/icon-tests4.log`,
`local/icon-launcher-tests2.log`, `local/icon-input-tests.log`, and
`local/icon-native-build2.log` passed. The resource probe matched all seven
embedded PNG layers byte-for-byte in the full-build game/launcher, standalone
launcher and installed package executables. Reviewed the generated 256-pixel
image; the wide wordmark and original transparent margins are retained.
