# Windows release process

`VERSION` is the source for the three-part product/MSI version. Increment it for
every public build and tag `vX.Y.Z`. Never replace published tags or assets.
Keep experimental releases marked as prereleases. The MSI UpgradeCode is stable;
component identities derive from paths. Mutable settings are preserved.

## Asset-free CI

`.github/workflows/ci.yml` runs on PRs and pushes to main with read-only
permissions, Windows 2022, Python 3.12.10, LLVM 20.1.8, the committed SDK gitlink,
and checksum-pinned SDL source. Logs survive failures for 14 days. No game files,
firmware, generated game translations, secrets or self-hosted runners are needed.

Run `git submodule update --init --recursive`, then
`./scripts/ci.ps1 -Python (Get-Command python).Source` in an asset-free checkout.
The script rejects dirty/mismatched SDKs and local game/generated inputs. It runs
the runtime, launcher, input/controller/UI, icon, Steam, setup and packaging
regressions serially because existing suites share some fixture paths.

The hosted image supplies serviced MSVC libraries and Windows SDKs; it is not
immutable. Logs record ImageVersion, SDK revision and CMake compiler detection.
Source/compiler pins do not imply bit-identical Windows builds.

Local-only checks: full game compilation, generated guest patch guards, disc
movie decode/timed playback, ISO/firmware preparation, actual gameplay/save/load,
controller hardware, display changes and final clean-Windows acceptance.
Public CI green does not establish that these passed.

## Build and package

Use a clean release checkout and local supported assets/translations:

```powershell
./scripts/build.ps1 -Configuration Release -Test
./scripts/build-launcher.ps1 -Configuration Release -Test
./scripts/prepare-release-tools.ps1
./scripts/stage-runtime.ps1 -Game build/haze/HazeRecomp.exe `
  -Launcher build/launcher/bin/HazeLauncher.exe -OutputDirectory build/runtime-stage
dotnet tool install wix --version 6.0.2 --tool-path build/wix
python scripts/release.py --runtime build/runtime-stage --tools build/release-tools `
  --licenses local/release-licenses --notes local/release-notes.txt `
  --output build/release-0.1.0 --wix build/wix/wix.exe
```

Prepare tools into a new directory; never package an existing RPCS3 installation.
The script downloads official hash-pinned archives and records extracted-file
hashes. Do not execute RPCS3 in this pristine tree. Changes to helper/firmware pins
require repeating first-run validation.

Runtime staging collects transitive non-system DLLs and the app-local VC runtime;
missing dependencies fail staging. Packaging admits only the staged binaries,
pinned helper trees, configuration, input-prompt assets, setup scripts, licenses,
notes, version and hash manifest. It excludes development launcher defaults,
generated sources, game/firmware inputs, build trees, saves and local settings.
Review the manifest. The staging metadata records the checkout commit but cannot
prove an arbitrary supplied executable's origin; build those binaries yourself.

Outputs are a per-user x64 MSI, portable ZIP and SHA256SUMS.txt. ZIP entry order
and timestamps are fixed; bit-reproducible MSI/compiler output is not claimed.
If signing, sign executable inputs before staging and the MSI before final
hashing/acceptance. Unsigned releases must identify that in their notes.

## Redistribution requirements

Supply actual license texts plus THIRD-PARTY-NOTICES.txt and SOURCE-OFFER.txt
in local/release-licenses. These are required inputs, not placeholder assurances.
Cover project/SDK notices, input-prompt artwork, SDL, FFmpeg and its dependencies,
VC runtime redistribution, 7-Zip, RPCS3 and the libraries in its Windows archive
(including Qt/FFmpeg). Review upstream build configurations and versions.

[RPCS3 is GPLv2](https://github.com/RPCS3/rpcs3/blob/master/LICENSE). Its separate
unmodified CLI executable is intended as aggregation. Preserve license/notices
and provide complete corresponding machine-readable source for the exact binary,
including required submodules, dependency source and build scripts, alongside
the release, or fulfill a compliant written source offer. A generic GitHub link
alone is not a source-distribution plan. SOURCE-OFFER.txt describes the actual
fulfillment method; publishing also requires a reviewed corresponding-source
archive. Fulfill the LGPL source/relinking obligations of bundled libraries.

Firmware is downloaded from Sony by the user or supplied locally, never bundled.
Review embedded/generated executable content and its redistribution provenance
before approving binary publication. PR CI never creates public releases.

## Acceptance gates and publishing

Validate the exact MSI/ZIP hashes to be published. Fresh custom paths plus a PATH
containing only Windows directories test independence from development tools.
They do not prove absence of machine-wide DLLs; also validate a clean Windows
installation without RPCS3 or development tools before a public release.

- Install as a normal user and launch via Start and another working directory.
  Exercise missing/encrypted/wrong-region ISO, bad firmware, cancellation,
  network failure, hash mismatch and corrupt helper output.
- Complete ISO + Sony-download preparation and offline firmware preparation.
  Verify title, movies, campaign start, audio, input, save, quit and second-session
  load. Record OS/GPU/driver and evidence paths.
- Test spaces/Unicode paths and reruns after failure. Verify old configuration
  and prepared data survive failures and unrelated RPCS3 installs are untouched.
- Customize an older MSI installation, create a save, then upgrade, repair,
  uninstall and reinstall. Check preservation and downgrade blocking. Test ZIP
  extraction separately.
- Review notes, package manifest, notices, source archive and redistribution.

Record these results in docs/release-acceptance.example.json, replacing false
gates with actual evidence links and matching hashes. After public CI passes
for the release commit, push its annotated tag and create a draft:

```powershell
./scripts/publish-release.ps1 -Directory build/release-0.1.0 `
  -Evidence local/release-acceptance.json -Notes local/release-notes.txt `
  -SourceArchive local/corresponding-source.zip
```

The publisher validates evidence, hashes and the remote tag commit and uploads
the MSI, ZIP, checksums, evidence and source archive to a **draft prerelease**.
Review that concrete draft before publishing in GitHub. It never overwrites tags
or releases. Tooling/tests in this PR are not a clean-Windows or release approval.
