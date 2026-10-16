# Windows binary releases

`.github/workflows/release.yml` builds on GitHub-hosted Windows 2022 runners.
Pushing `vX.Y.Z` builds the tagged code, fetches the pinned private generated
inputs, compiles the game and dependencies, packages the installer, uploads all
assets to a draft, verifies the uploads, then publishes the completed release.
No developer PC or self-hosted runner is required.

## One-time configuration

The private repository `zdiemer/haze-build-inputs` holds only the generated C/C++
and headers needed to compile. It has a read-only deploy key whose private half
is the `BUILD_INPUTS_SSH_KEY` Actions secret in this repository. Do not enable
write access or expose this secret to pull-request workflows. This configuration
is already provisioned for the main repository; forks must supply their own.

`config/build-inputs.json` pins the private repository, full input commit and SDK
gitlink. The snapshot includes a SHA-256 manifest verified before building.
There are no ISO/ELF/BIN files or game assets in this snapshot. The installer
creates the runtime EBOOT.ELF and game files from the user's ISO.

To refresh inputs, export a new local directory:

```powershell
python scripts/build_inputs.py --export generated --output local/new-build-inputs
python scripts/build_inputs.py --verify local/new-build-inputs
```

Commit the resulting snapshot to the **private** repository. Update the full
commit and matching SDK revision in `config/build-inputs.json` through a PR.
Never make the private repository public or upload it as an Actions artifact.
Private storage does not itself establish redistribution rights for a compiled
game-derived executable.

## Cut a release

1. Update `VERSION` (three numeric fields) and `docs/release-notes.txt` in a PR.
   Merge it and wait for the asset-free Windows checks to pass.
2. Push an annotated tag at that main commit:

   ```powershell
   git tag -a v0.1.1 <main-commit> -m 'Haze Recomp 0.1.1'
   git push origin refs/tags/v0.1.1
   ```

3. Watch **Windows release** in Actions. It publishes only after the complete
   artifact set is built and verified. A tag must match `VERSION` and refer to
   a commit on main. Existing releases and assets are never overwritten.

For an existing tag, use **Actions > Windows release > Run workflow**, enter
the tag, and enable `publish`. Without that checkbox the workflow builds and
retains the deliverables as an Actions artifact without publishing a release.
This supports the existing `v0.1.0` tag without moving it. The workflow version
provides packaging/installer tooling while game code comes from the chosen tag.

If publication fails after upload, keep the draft and download the exact
`windows-release` artifact from the failed run. The publisher resolves drafts
through authenticated release listings and uses their numeric IDs; GitHub's
release-by-tag endpoint only finds published releases.

For the v0.1.1 publication failure, run the following from a checkout containing
the publisher fix (with Python and authenticated `gh` available):

```powershell
gh run download 38501319172 --repo zdiemer/haze-recomp --name windows-release --dir local/recover-v0.1.1
./scripts/publish-release.ps1 -Directory local/recover-v0.1.1 -Tag v0.1.1 `
    -Commit 2139a6556ba51444a650c19395aba004b4ce14d6 -Notes docs/release-notes.txt `
    -ResumeDraftId 412290693 -VerifyOnly
```

After read-only verification succeeds, repeat the publisher command without
`-VerifyOnly` to publish that draft. It rechecks the tag, exact asset set, sizes
and SHA-256 digests before publishing by ID. Resume never uploads or replaces
files, rewrites notes, or modifies an already published release. An incomplete
or mismatched draft fails verification and needs investigation. For another run,
substitute its run ID, tag, commit and draft ID.

Rerunning the old failed job uses its original broken script; rebuilding can
produce different MSI bytes. Recover using the original artifact and the fixed
publisher instead. Do not delete or move the tag. A published version is
immutable: fix problems in a new version instead.

## What gets built and published

- `HazeRecomp-X.Y.Z-Setup.exe`: the complete interactive installer. It embeds a
  per-user MSI and temporary preparation tools. It installs the launcher/game,
  prepares the user's ISO and firmware, validates the launcher, then offers Play.
- `HazeRecomp-X.Y.Z-windows-x64.zip`: portable application files for users with
  already prepared disc/firmware paths; it does not perform first installation.
- `HazeRecomp-dependency-sources.zip`: exact source archives, project/SDK sources,
  FFmpeg configure log and the dependency build recipe.
- `release.json` and `SHA256SUMS.txt`: commit/input provenance and artifact hashes.

Only this explicit set is uploaded. The internal MSI, build directory, logs,
generated sources and keys are excluded. Compiler diagnostics can contain private
source excerpts, so build logs stay on the runner and are not public artifacts.
When investigating a build failure, reproduce that step locally with the private
snapshot. There is no cache of private input files or build objects.

## Dependencies and notices

LLVM, WiX, Python, action revisions, SDK, input snapshot, tool archives and source
archives are pinned. Windows SDK/MSVC and MSYS2 system tools are serviced by the
runner image; bit-for-bit reproducibility of the executable/MSI is not claimed.

FFmpeg DLLs are built from the exact pinned source with MPEG-2 video, ATRAC3+
audio, scaling and resampling enabled. External libraries, GPL and nonfree
features are disabled. This avoids redistributing an opaque prebuilt FFmpeg
dependency collection. FFmpeg remains dynamically linked and replaceable.
The matching source archive and build recipe are shipped alongside the binary.

7-Zip is bundled with its source and notices, including the unRAR restriction.
SDL, project/SDK and input-prompt licenses are included, together with Microsoft's
installed distributable-code list for the app-local VC runtime.
Networking dependencies are pinned as well: libdatachannel, libjuice, Mbed TLS,
usrsctp, plog and nlohmann/json. Their source archives and notices are included.
The source bundle also contains the exact modified libdatachannel WebSocket
source that enables Windows certificate verification, and its guarded build
recipe in the project archive.

RPCS3 is downloaded directly from the pinned official release during installation
and verified before execution. The MSI/ZIP contain neither RPCS3 nor Sony firmware.
Firmware is downloaded from Sony and checksum-verified. No placeholder notices
or unfulfilled source offers are used by the automated packager.

When RPCS3's required system runtime is missing, installation also downloads
Microsoft's pinned Visual C++ redistributable, verifies its checksum and signature,
and requests administrator approval to install that prerequisite.

## Validation

PR CI covers existing asset-free runtime/launcher/UI tests, private-input filtering,
package integrity, tag/commit matching, and compilation of the actual installer
with synthetic payloads. The release build compiles the complete game from the
private snapshot, runs launcher tests and verifies installer payloads/checksums.

Before promoting a version, exercise the installer with the supported ISO on a
fresh Windows environment, then test movies, campaign start, audio, input,
save/load and upgrades. Hosted compilation is not evidence that a full campaign
or every GPU works. Record actual limitations in the release notes rather than
marking an unperformed acceptance test as passed. Releases are currently unsigned.
