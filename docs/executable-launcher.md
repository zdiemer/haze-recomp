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
game. `--diagnostics` and `--export-diagnostics FOLDER [--session NAME]` export
a report (see below). None is required for normal play.

## Diagnostics reports

**Export diagnostics...** creates one local ZIP for an issue report. It is
offered beside **Open session logs** whenever a launch fails or the game exits
with a nonzero code. For a problem that does not end in an error (a hang,
rendering or audio fault, or a crash from an earlier run), use the **Haze Recomp
diagnostics** Start menu shortcut or run `HazeLauncher.exe --diagnostics`. It
lists the ten newest sessions with their recorded status; choose one, then a
destination folder. Exporting runs in a fresh launcher process after the game
has exited, so a run that crashed before the menu can still be exported. It
does not require a launchable configuration: an invalid setting (for example an
unknown InputMode) or malformed INI lines are skipped, defaults are used for the
affected values, and each validation error is listed in `summary.txt` and
`settings.txt`. The raw INI files are included as they are. Without dialogs:

```powershell
HazeLauncher.exe --no-ui --export-diagnostics D:\reports              # newest session
HazeLauncher.exe --no-ui --export-diagnostics D:\reports --session 20261015-120000-...
```

The report is `haze-diagnostics-<session>.zip`. An existing report is never
replaced: a repeat export becomes `-2.zip`, `-3.zip` and so on. The ZIP is staged
beside the destination and published only after it is completely written and
flushed, so a read-only folder or full disk reports the Windows error, offers
another folder and leaves earlier reports and session logs unchanged. Nothing is
uploaded. The archive is uncompressed so Explorer, `tar` and text editors can
inspect it directly.

| File | Contents |
| --- | --- |
| `summary.txt` | Session name and status (normal exit, exit code with its NTSTATUS name such as access violation, did not start, or incomplete), `haze_crash_filter` context and guest LR lines from stderr, the last `[haze-boot]` progress line, build identification, game revision and path traits |
| `manifest.txt` | Each item and whether it was included, truncated, missing, unreadable or omitted for size |
| `system.txt` | Windows version/build, architecture, CPU, memory, primary display mode, and each DXGI adapter with PCI IDs, video memory and driver version, collected at export time |
| `settings.txt` | Every launcher setting resolved at export time with its source (command line, environment variable, launcher configuration, generated or built-in default), and the HAZE_/RSX_/PS3_/SDL_/YDKJ_ variables passed to the game, marked inherited or set by the launcher |
| `config/` | `launcher.ini`, `launcher-defaults.ini`, the input INI and graphics INI as configured (current contents) |
| `build/build-info.json` | Release provenance (version, source and SDK commits, file hashes) when the package has it |
| `session/` | `launcher.log`, `game.stderr.log`, `game.stdout.log` and other text files from the session |

`launcher.log` now records `Finished`, the source of each resolved setting and
the diagnostics-relevant environment overrides used by that launch, so the
session's own values remain distinguishable from current configuration. A
session whose launcher never recorded its exit is reported as incomplete.

While the game runs, the launcher also writes `build-identity.txt` into the
session: the executable's SHA-256, PE timestamp/SizeOfImage and PDB GUID/age, and
EBOOT.ELF's size and SHA-256 with its PARAM.SFO fields. Hashing happens while the
running image prevents the executable from being replaced, and does not delay
the game's start. Reports use this recorded identity for the session's build.
They list the files currently on disk separately, as `CurrentOnDisk.*` with
`CurrentOnDisk.MatchesSession`, so an executable rebuilt or updated since the
session cannot be mistaken for the one that ran. Sessions recorded before this
change report their build as unknown.

Build identification combines the launcher's version, source commit and SDK
gitlink (recorded when the launcher build is configured; `-modified` marks
uncommitted tracked changes), the launcher's SHA-256, the session's recorded game
identity above, and whether that game matches `build-info.json`. Game data is identified by EBOOT.ELF's
size and SHA-256 and the PARAM.SFO TITLE_ID/VERSION/APP_VER fields; the supported
title is BLUS30094. A developer can therefore match the report with a release
build and its PDB without the user's game files.

Size is bounded: each log keeps at most 4 MiB (its first 1 MiB and the final
bytes, where crash context is written, with an omission marker), configuration
files 256 KiB, at most 32 session text files, and the whole archive about
32 MiB. Later logs are omitted, and listed in the manifest, once that limit is
reached. Session folders are not deleted or rotated by the export.

### Privacy

Before archiving, every text file is redacted: the InstallRoot and SharedRoot
folders, the user profile (`USERPROFILE`, `HOMEDRIVE`/`HOMEPATH`) and OneDrive
folders become `<InstallRoot>`, `<SharedRoot>`, `<UserProfile>` and `<OneDrive>`,
and the Windows user, computer and domain names become `<user>`, `<computer>` and
`<domain>` where they appear as whole words. Matching ignores ASCII case and path
separator style; names shorter than three characters and drive roots are left
alone. Path length, spaces and non-ASCII characters in InstallRoot are reported
instead. Only the PDB file name is kept, not its build path.

License keys and identities are never written to `launcher.log` and never
exported: the EDAT license key (`PS3_RIF_KEY`), console PSID (`PS3_OPEN_PSID`),
NP online ID (`PS3_NP_ONLINE_ID`) and PSN relay host (`PSNR_SERVER`), plus any
variable whose name contains a credential-like word (KEY, RIF, RAP, IDPS, PSID,
KLIC, ACT, LICENSE, AUTH, TICKET, TOKEN, SECRET, PASSWORD, CREDENTIAL and
similar). They are recorded as `<redacted>` so their presence stays visible. At
export, every `NAME=value` assignment of such a variable in any collected file
is replaced too, even when the variable is no longer set, so an older
`launcher.log` that recorded a key is scrubbed. The current values of these
variables are also replaced wherever the game printed them. `.rifkey` and other
non-text files are never collected.

Redaction is best effort. It does not recognize other names (for example a
Steam persona or a folder named differently from the account), short 8.3 path
forms or text the game writes in another encoding. Review the files before
sharing them.

The report never includes game or firmware files, EBOOT/ELF contents, saves,
the shader cache, keys, non-text session files, environment variables outside
the prefixes above, or the values of the key and identity variables above.
Minidumps are not written or collected. `haze_crash_filter` runs inside the
failing process and already records the exception, registers, last HLE call and
guest link registers; a full or heap dump would contain guest memory with
decrypted game code and data and possibly personal data, so any future optional
dump needs a separate, explained opt-in and an out-of-process writer.

## Evidence and remaining acceptance

`HazeLaunchTests` runs the real Windows process creation code with an owned
fixture executable and DLL. It covers configuration precedence, a different
working directory, spaces and an accented path, Windows argument quote
round-trips, atomic/locked configuration writes, input validation, an empty
PATH (no shell lookup), no console window, restricted handle inheritance,
persistent paths across launches, separate logs, actual child waiting and exit
code propagation. The real GUI-subsystem launcher is also invoked with
`--no-ui`, including an actual missing imported DLL (`0xC0000135`). Fresh
launcher processes then export the previous failed runs with
`--export-diagnostics`, and an unknown session is rejected.

`HazeLaunchDiagnosticsTests` covers the collection logic without dialogs:
archive structure and CRCs (checked by an independent reader and, when present,
Windows' bundled `tar`), redaction of profile/root paths, user and computer
names across separators and case, head/tail log bounds, the overall size limit,
session ordering and status, missing and non-text files, setting sources and the
environment allowlist, PARAM.SFO and PE symbol identification, and exports that
never replace an existing report or leave a partial file. Regressions cover a
historical launcher.log containing `PS3_RIF_KEY` after the variable is unset,
key and PSID values in settings and game output, and an executable replaced
between launch and export (recorded versus current-on-disk identity, and an
unrecorded session reported as unknown). HazeLaunchTests also checks that a
launch never writes the key to `launcher.log` and records `build-identity.txt`,
and that the real `HazeLauncher.exe --no-ui --export-diagnostics` exports an
existing session with an invalid InputMode or a malformed INI, while `--check`
still rejects both. The Explorer dialogs
and **Show in folder** have not been exercised interactively, and no export has
been made from a real game crash.

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

The installed launcher (SHA-256
`4797964063d94fc94773e4231ced2687028ccf4808be0439a17a1875fb8dd937`)
also passed real TaskDialog and Windows file-picker checks on a private desktop.
Starting without a game executable opened the executable picker; subsequent
disc and firmware folder selections were saved to the isolated `launcher.ini`.
The selected process fixture then ran with those paths and deliberately returned
7. The launcher displayed its failure dialog with **Open session logs**, recorded
the error in `launcher.log`, and returned the same exit code after Close.
Canceling the executable picker and selecting the launcher itself as the game
both left the prior configuration unchanged and started no child. The invalid
selection returned to the repair prompt after its error was dismissed.

These checks used posted messages addressed only to the owned launcher's native
dialogs. They did not switch the visible desktop, run the game, or activate the
log-opening button. Evidence: ignored `local/picker-native-2/report.json`,
`local/picker-native-3-cancel/report.json`, and
`local/picker-native-4-invalid/report.json`. The first probe had an incorrect
fixture INI section; a later harness assertion sampled the closing dialog before
its replacement appeared. Both were harness defects, with no production change.

Still required: the game's embedded icon from #41; interactive Explorer launch
and log-opening checks; actual Steam desktop/Big Picture launch and lifetime
reporting; and normal audio confirmation. The native game
and SDK still have their own path-length/encoding limits; UTF-16 launcher fixture
coverage is not proof that every game subsystem accepts every Unicode path.

Implementation references: Microsoft's [CreateProcessW documentation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
and [redirected standard I/O example](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output).
