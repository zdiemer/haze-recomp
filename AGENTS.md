# Haze Recomp

Experimental Windows x64 port of Haze (PS3 USA BLUS30094, disc 01.02,
application 01.00). Read `README.md` and `scripts/README.md` for setup and
commands; `docs/releasing.md` describes release validation. Browser support
under `port/wasm` is experimental.

## Repository layout

- `port/`: native game integration, launcher, platform and networking code.
- `external/ps3recomp/`: Git submodule containing the runtime and lifters.
- `scripts/`: build, lifting, packaging and diagnostics; generated-code
  transforms live in `scripts/patches/` behind `scripts/patch.py`.
- `tests/`: asset-free and prepared-game regressions; `config/`: defaults.
- `game/`, `generated/`, `build/`, `local/`, `.venv/`, `.worktrees/`: ignored
  local inputs and outputs. Never commit game/firmware binaries or lifted code.

## Working and checking

Start changes in a worktree from freshly fetched `origin/main`. Preserve other
worktrees and their local SDK checkouts. Use Windows PowerShell, VS C++ tools,
Clang, CMake/Ninja and Python 3.11+. Initialize the pinned SDK with
`git submodule update --init --recursive`.

Run focused regressions for the changed behavior. `scripts/ci.ps1 -Python
<python.exe>` runs asset-free Windows checks; `scripts/build.ps1 -RuntimeOnly
-Test` builds/checks the SDK without game inputs. A prepared checkout can be
reused with `scripts/build.ps1 -SharedRoot <checkout> -Test` and
`scripts/play.ps1 -SharedRoot <checkout>`. Initialize this worktree's SDK or
pass `-SdkRoot` explicitly so validation uses the intended revision. Keep
builds/logs local to the worktree. Report gameplay or device checks separately
from synthetic tests; do not claim full campaign compatibility from a boot test.

## SDK branch and pin hygiene

The SDK fork is **zdiemer/ps3recomp**, integration branch **haze**. Its upstream
is **sp00nznet/ps3recomp**, branch **master**. Keep `.gitmodules` pointing at
`https://github.com/zdiemer/ps3recomp.git` with `branch = haze`.

The gitlink is an immutable commit, not a floating branch. `branch = haze`
only guides `git submodule update --remote`; it does not validate a pin.
Every SDK commit pinned by this repository must already be reachable from
the published `zdiemer/ps3recomp:haze` branch. A detached SDK HEAD from normal
submodule initialization is expected and is not a hygiene violation.

1. Develop SDK changes in an isolated SDK worktree. Preserve the currently
   shipped pin and all Haze changes; do not replace it with upstream master.
2. Inspect upstream changes for equivalent Haze fixes and dependencies. Use
   `git cherry-pick -x` for selected fixes and run their regressions plus the
   relevant Haze checks. Record deferred changes and reasons.
3. Integrate and publish validated SDK commits to `haze` before committing a
   new parent gitlink. Preserve history; do not force-push or rewind `haze`.
   Experimental branches may exist, but do not pin commits unique to them.
4. Commit the exact SDK SHA in the parent, then run
   `python scripts/check_sdk_pin.py --fetch`. It checks the committed gitlink,
   clean SDK checkout, canonical configuration and ancestry on fetched `haze`.
   CI and release builds enforce the same rule. A pin may lag `haze`; it need
   not equal the branch tip. Revalidate after changing either repository.

See `docs/sdk-upstream.md` for the upstream review and selected fixes.
