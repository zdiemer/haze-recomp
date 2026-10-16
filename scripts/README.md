# Development scripts

Run commands from the repository root. Python tools require Python 3.11+;
native build tools use Windows PowerShell and Visual Studio C++ tools.
Use `python scripts/diagnose.py --help` to list diagnostics and append
`--help` to a command to see its inputs.

## Workflows

| Task | Entry points |
| --- | --- |
| Prepare game files | `extract.ps1`, `decrypt.ps1`, `haze.py inventory` / `analyze` |
| Generate translations | `haze.py lift` / `lift-spu`, `lift_audio_plugins.py`, `lift_firmware.py` |
| Build and check | `build.ps1`, `build.ps1 -Test`, `ci.ps1` |
| Verify SDK integration branch | `check_sdk_pin.py --fetch` (committed pin must be on `zdiemer/ps3recomp:haze`) |
| Play or collect a bounded run | `play.ps1`, `run.ps1 -Seconds 90` |
| Inspect captures and performance | `diagnose.py <command>` |
| Native microbenchmarks | `benchmark.ps1 <benchmark>` |
| Browser development | `bootstrap-wasm.py`, `build-wasm.ps1`, `serve-wasm.py`, `playtest-wasm.py` |
| Package releases | `build-release.ps1`, `publish-release.ps1`; see [release documentation](../docs/releasing.md) |
| Asset maintenance | `haze_pak.py`, `import-input-prompts.py`, `build-icon.ps1` |

Build, setup, release and test helpers retain their established paths because
CMake, CI, the installer and release packaging call them directly.

## Diagnostics

```powershell
python scripts/diagnose.py renderer local/runs/<run>/renderer.csv --last 600
python scripts/diagnose.py browser-cpu local/wasm-tests/<run> --json
python scripts/diagnose.py present-modes --help
python scripts/diagnose.py native-cpu --help
```

`diagnostics/` contains the implementations and shared helpers. Tests import
these modules; command-line users use `diagnose.py`. Imports are lazy, so
offline reports can run without loading Windows process APIs. Existing report
arguments and formats are preserved. Sampling, logging and readbacks perturb
timing; follow each command's collection requirements.

| Former script prefix or name | Command |
| --- | --- |
| `analyze_*` | Name without the prefix, using dashes; `analyze_producer_profile` becomes `producer` |
| `summarize_*` | Name without the prefix, using dashes; `summarize_profile` becomes `renderer` |
| `correlate_pump_stacks`, `correlate_pump_waits` | `pump-stacks`, `pump-waits` |
| `benchmark_cpu_work`, `benchmark_present_modes`, `benchmark_thread_priority` | `cpu-work`, `present-modes`, `thread-priority` |
| `capture_sequence`, `compare_draw_frames`, `detach_pad_file` | `capture`, `compare-draws`, `detach-pad` |
| `profile_native_cpu`, `watch_frame_progress` | `native-cpu`, `watch-frames` |

Build the optional stack-unwinding helper with `build_profiler.ps1 -Test`
before using `native-cpu --stack-depth`. Process observation for present-mode
benchmarks remains available through `observe_haze_processes.ps1`.

## Native microbenchmarks

The three former `benchmark_*.ps1` wrappers share compiler setup and error
handling in `benchmark.ps1`:

```powershell
.\scripts\benchmark.ps1 spu-abi -Heap
.\scripts\benchmark.ps1 vertex-fetch -Modes 3 -Tag current
.\scripts\benchmark.ps1 write-watch
```

Outputs live under `local/benchmarks/<benchmark>`. Use `-SdkRoot` to select a
prepared SDK in another checkout. Vertex-fetch requires a built runtime;
`-RuntimeLibrary` selects its `.lib` when using a shared checkout.

## Build internals

`patch.py <patch> <source> <output>` handles argument parsing, file I/O and
SPU registration checks for generated-code transforms in `patches/`.
CMake invokes it automatically and tracks the tool and its modules as build
dependencies. Hash guards, transform ordering and each patch's original text
encoding remain intact. Build-local outputs leave the shared lift untouched.

Some patches need additional inputs; use `patch.py <patch> --help`. Private
regression fixtures are exposed through `patch.py oracle-<name>`. They remain
separate implementations because their extraction boundaries and source hashes
are independently audited. No extracted game code is checked in.

## Removed investigations

These tools had no build, CI or regression callers and were specific to past
investigations. Their sources remain in Git history.

| Removed tool | Reason |
| --- | --- |
| `analyze_deck_sequence.py` | Fixed carrier-deck crop and paint-color experiment; general capture and draw comparison remain |
| `sample_haze_clock.py` | Candidate clock globals at fixed guest addresses |
| `observe_ref_counters.py` | One-off probe of selected renderer reference-counter symbols |
| `profile_spu_image.py` | Separate image-specific sampler; retained native CPU profiling supports selected thread IDs and stack unwinding |
| `measure_thread_cpu.py` | Standalone control-flag/thread-counter experiment; retained CPU-work and present-mode tools cover repeatable comparisons |
| `prepare_spu_drain_baseline.py` | Extracted a historical SDK implementation for a retired comparison; its unused `tests/benchmark_spu_drain.c` companion was also removed |

Add repeatable diagnostics as modules behind `diagnose.py`, with reusable
parsing and relevant regression coverage. Keep temporary investigation scripts
under ignored `local/` rather than adding another top-level command.
