# Native Windows launcher

`HazeLauncher.exe` starts the native game directly. Normal play does not invoke
PowerShell, `cmd.exe`, a script association or an elevated process. The launcher
has the Windows GUI subsystem and starts the game's console-subsystem executable
with `CREATE_NO_WINDOW`; Haze's own graphics window still opens normally.

Build with `scripts/build.ps1` and double-click `build/haze/HazeLauncher.exe`.
The launcher is built alongside Haze and installed with the native package.
To build/test only the launcher, without rebuilding the translated game:

```powershell
scripts/build-launcher.ps1 -SharedRoot E:/Code/haze-recomp -Test
```

Standalone build/test artifacts live in `build/launcher/bin`, separate from the
full native build's outputs. This target still needs a built native game before it can play;
use `--executable` or configure Executable when running that standalone binary.
`scripts/play.ps1` remains available for development and diagnostics.

## Paths and configuration

Development builds generate `launcher-defaults.ini` beside the executable. Its
relative InstallRoot points back to the source worktree; SharedRoot records the
build's `-SharedRoot`. Existing `local/state/savedata`, `local/cache/rsx-shaders`,
`config/input.ini`, optional `config/graphics.ini` and `local/sessions` therefore
match `play.ps1` for that worktree. A whole development tree can move without
changing its relative InstallRoot. Explicit paths to another shared data tree
still need to identify that tree after a move.

Packages omit these development defaults and use the executable's directory as
InstallRoot. They include the input configuration and input-prompt assets via
the game's existing install rules. Game data and firmware are never bundled.

Copy `config/launcher.example.ini` beside the launcher as `launcher.ini` to
customize a launch. Files are UTF-8. Do not put quotes around INI values;
spaces and Unicode are accepted by the launcher. InstallRoot is resolved
relative to the launcher directory. All other relative configured paths resolve
against InstallRoot, independent of Explorer's or Steam's working directory.
Drive-relative forms such as `C:game` are rejected.

Precedence is explicit command-line option, corresponding inherited environment
variable, nonempty `launcher.ini` setting, generated build default, built-in
default. Empty INI values retain defaults. `--config` or `HAZE_LAUNCHER_CONFIG`
selects another configuration file (relative to the launcher directory); an
explicitly selected missing file is reported instead of silently ignored.

| Setting / command-line option | Environment override | Default |
| --- | --- | --- |
| InstallRoot / `--install-root` | HAZE_INSTALL_ROOT | Launcher directory or development default |
| SharedRoot / `--shared-root` | HAZE_SHARED_ROOT | InstallRoot |
| Executable / `--executable` | HAZE_EXECUTABLE | HazeRecomp.exe beside launcher |
| DiscDirectory / `--disc-directory` | PS3_VFS_ROOT | SharedRoot/game/disc |
| Firmware / `--firmware` | PS3_DEV_FLASH | SharedRoot/local/tools/rpcs3/dev_flash |
| Savedata / `--savedata` | PS3_SAVEDATA_ROOT | InstallRoot/local/state/savedata |
| ShaderCache / `--shader-cache` | RSX_SHADER_CACHE_DIR | InstallRoot/local/cache/rsx-shaders |
| Sessions / `--sessions` | HAZE_SESSION_ROOT | InstallRoot/local/sessions |
| InputConfig / `--input-config` | HAZE_INPUT_CONFIG | InstallRoot/config/input.ini |
| GraphicsConfig / `--graphics-config` | HAZE_GRAPHICS_CONFIG | InstallRoot/config/graphics.ini if present |
| InputMode / `--input-mode` | HAZE_INPUT | Auto; also KeyboardMouse/keyboard or Controller |
| RenderResolution / `--render-resolution` | RSX_RENDER_RESOLUTION | No launch override; graphics configuration decides |

RenderResolution accepts canonical `WIDTHxHEIGHT`, with each dimension 1–8192.
Unknown options/settings, duplicate keys, malformed encoding and oversized
configuration files produce errors. Other INI sections are retained during
first-run path updates.

The launcher supplies the normal play defaults for online emulation, CRI
initialization, unresolved-HLE failure, quiet diagnostics and live rendering.
Other inherited variables remain available for diagnostics, including explicit
audio overrides; the launcher does not mute audio by default.

If the game executable, decrypted ELF/disc data or firmware is missing, a native
dialog offers to select the corresponding location. The disc selector expects
the folder containing `PS3_GAME`; the firmware selector expects `dev_flash`.
The selected path is validated and saved to `launcher.ini` with a flushed sibling
file and atomic replacement. A failed save leaves the original configuration
intact. Selecting a path overrides an inherited value for that launch; inherited
environment overrides will still take precedence on a later launch, as documented
above. Clear stale environment overrides if a saved path appears to be ignored.

The launcher checks the 64-bit executable, EBOOT.ELF, PARAM.SFO, firmware
`sys/external/flashMP3.pic`, input INI and an explicitly selected graphics INI.
It rejects selecting itself as the game. Runtime DLLs are checked by Windows'
loader: the normal source build links SDL statically, so a separate SDL3.dll is
not universally required. A missing or wrong-architecture DLL is reported from
the real child exit code, with an instruction to reinstall the runtime files.

The installation/state/config folders must be writable where changes are needed.
The launcher reports permission errors and never requests elevation or silently
switches existing progress to a different save folder. A protected installation
can use configured writable state/cache/session paths and a writable `--config`
file; a portable writable installation is the simplest setup.

## Logs, process lifetime and Steam

Each launch creates a unique UTC/PID session directory with `launcher.log`,
`game.stdout.log` and `game.stderr.log`. The report records resolved paths and
the final exit code, not the complete inherited environment. Haze runs in that
session directory, with persistent save/cache/settings paths passed explicitly.
Only its three standard I/O handles are inherited; unrelated inheritable handles
in the launcher are excluded with a process attribute list.

The launcher waits for the exact game process handle, then returns its exit code.
Zero is normal completion. A failed launch or nonzero game exit presents an
actionable error and an **Open session logs** button. It does not infer success
from elapsed time or terminate a long-running game. This keeps Steam's tracked
launcher alive for the game's full lifetime.

The launcher exports its absolute path as `HAZE_LAUNCHER_PATH` and a canonical
argument string as `HAZE_LAUNCHER_ARGUMENTS` for #47's registration menu. The
arguments retain the resolved data, save, cache, configuration paths and input
mode even when the initial launch used overrides that Steam would not inherit.
Repair the Steam entry after moving paths or changing these launch preferences.
Normal saved input/graphics changes continue to use the same INI files.

For diagnostics, `--no-ui` prints errors to inherited stderr and suppresses
dialogs; `--check` validates the resolved configuration without starting the
game. Neither is required for normal play.

## Evidence and remaining acceptance

`HazeLaunchTests` runs the real Windows process creation code with an owned
fixture executable and DLL. It covers configuration precedence, a different
working directory, spaces and an accented path, Windows argument quote
round-trips, atomic/locked configuration writes, input validation, an empty
PATH (no shell lookup), no console window, restricted handle inheritance,
persistent paths across launches, separate logs, actual child waiting and exit
code propagation. The real GUI-subsystem launcher is also invoked with
`--no-ui`, including an actual missing imported DLL (`0xC0000135`).

A native probe used the tested game from the menu/Steam branch with an isolated
copy of the previously saved profile and paths containing spaces. The launcher
started PID 52680, retained its handle through a 100-second run, and returned 0
after the game's save admission, audio and renderer shutdown completed. No game
child remained. Evidence is in ignored `local/launcher native probe 2/`. The
probe used PAD_FILE and posted window close, with output audio deliberately
muted; it is not an Explorer/physical-input/listening acceptance check. The
first probe exposed and corrected the erroneous assumption that SDL3.dll was
always required.

A second successful native probe launched the freshly built main-based game
with no launcher arguments, using saved `launcher.ini` configuration from an
unrelated working directory. Game PID 19028 exited after the posted window close
at 100 seconds; the launcher returned 0 with no remaining child. Its report
confirmed the configured save, input, graphics and shared cache paths. Evidence
is in ignored `local/launcher native probe 3/`. This also used diagnostic input
and muted output, so it does not replace the interactive checks below.

The full native build and standalone launcher suite pass. A CMake install into
an isolated package directory includes both executables, the example launcher
configuration, input configuration and prompt assets, and excludes generated
development defaults.

The first test harness failures entered CRT assertion dialogs; an owned-process
stack sample confirmed `wassert`/`abort`/`MessageBox`. The harness now reports to
stderr without dialogs. Path normalization and a test-only CRLF/path-separator
comparison were corrected; subsequent suites pass.

Still required: the game's embedded icon from #41; interactive Explorer launch
and first-run picker/error/log-opening checks; actual Steam desktop/Big Picture
launch and lifetime reporting; and normal audio confirmation. The native game
and SDK still have their own path-length/encoding limits; UTF-16 launcher fixture
coverage is not proof that every game subsystem accepts every Unicode path.

Implementation references: Microsoft's [CreateProcessW documentation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
and [redirected standard I/O example](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output).
