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

## Temporary files and cleanup

Cleanup is part of completing every task; do it before the final handoff.
Temporary builds, worktrees and diagnostic captures previously accumulated
terabytes here. Do not leave another task's worth of disposable outputs behind.

- Reuse prepared game inputs with `-SharedRoot`; avoid copying game data,
  firmware, SDKs or generated translations into each experiment.
- Keep temporary outputs in a named task directory. Bound trace duration and
  file sizes; stop capture processes when the check ends. Inspect disk usage
  before and after large builds, captures or packaging runs.
- After validation, delete this task's disposable builds, staging directories,
  dumps, raw traces and duplicate captures. Keep only concise results and the
  minimum evidence needed for unresolved problems. An ignored file is not
  automatically disposable: preserve saves, settings, keys, shared inputs,
  SDK checkouts and uncommitted or untracked source work.
- Remove this task's temporary worktrees with `git worktree remove` once work
  is committed and safely retained on a branch or PR. Check nested SDK
  repositories first; preserve unpublished commits and local changes. Never
  force-remove dirty worktrees or delete other active tasks' files.
- Before recursive deletion, resolve each absolute target, verify that it is
  inside the intended task directory, and reject junctions/symlinks that could
  lead outside it. Use PowerShell `Remove-Item -LiteralPath`; do not run blanket
  `git clean -fdx` or remove all of `local/` or `.worktrees/`.
- Report what was cleaned and any large artifacts intentionally retained,
  with their location and reason. If a worktree must remain for ongoing work,
  remove its disposable outputs and explain why the checkout is retained.

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
3. Land every SDK change as a pull request into `zdiemer/ps3recomp:haze`,
   and merge it before opening the Haze change that needs it. Preserve history;
   do not force-push or rewind `haze`. Experimental branches may exist, but do
   not pin commits unique to them; work that should ship goes through a `haze`
   PR first.
4. In the Haze pull request, re-pin the gitlink to the head of `haze` after
   that merge, not to the feature-branch commit. If `haze` moved again before
   the Haze PR merges, re-pin to the new head and rerun the checks. Then run
   `python scripts/check_sdk_pin.py --fetch`. It checks the committed gitlink,
   clean SDK checkout, canonical configuration and ancestry on fetched `haze`.
   CI and release builds enforce ancestry, so a pin already committed stays
   valid as `haze` advances; only new pins must be its head.
5. After a pin change merges, run `git submodule update --init --recursive` in
   each checkout you build from, including the main checkout, so its SDK
   matches the committed gitlink. Check the SDK for local commits or changes
   first.

See `docs/sdk-upstream.md` for the upstream review and selected fixes.
