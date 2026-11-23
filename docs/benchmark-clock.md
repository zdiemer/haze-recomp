# Frame-locked benchmark clock

A diagnostic mode that runs gameplay on virtual time so a performance
comparison measures the same simulation on every build. It is inert unless the
environment variables below are set; normal launches keep the real clocks.

## Why

Haze derives each frame's simulation step from `sys_time_get_system_time`
(`func_0071F028` → frame tick `func_0068ECD8`). On real time, a faster build
simulates shorter frames and reaches scripted events (for example the later
checkpoint's counterattack) in fewer frames, so a wall-clock benchmark compares
different scenes. Under the benchmark clock every frame simulates exactly one
fixed step, whatever the host speed.

## Variables

| Variable | Meaning |
| --- | --- |
| `HAZE_BENCHMARK_CLOCK_US=<step>` | Virtual guest time advances `<step>` µs per guest frame (6944 for 144 Hz) and is frozen within a frame. `mftb` returns the frame's time plus 1 µs per read on each thread. |
| `HAZE_BENCHMARK_ESCAPE_US`, `HAZE_BENCHMARK_GAMEPLAY_ESCAPE_US` | If no frame boundary arrives for this much real time (default 100000), time resumes at the real rate so loading and timed waits cannot hang. Escapes are logged and recorded. |
| `HAZE_BENCHMARK_TIMELINE=<settle>,<sweep>,<csv>` | Count gameplay frames from the first local-player look update; run the camera sweep for `<sweep>` frames after `<settle>`; then write `<csv>` and `<csv>.done`. Also resets the frame tick's accumulated game time and frame counter at gameplay frame 0. |
| `RSX_FRAME_FINGERPRINT_CSV=<csv>` | Per presented frame: draw count, transform-constant count and a hash of the constants (object matrices). Written at normal shutdown, plus a 60-frame constant trace from gameplay frame 0 in `<csv>.trace.csv`. |
| `HAZE_BENCHMARK_INPUT="<boundary>:<command>;..."` | Write `PAD_FILE` commands at guest frame boundaries instead of wall-clock times. |
| `HAZE_BENCHMARK_SYNC_GPU=1` | Drain the guest FIFO at each gameplay boundary. Removes renderer-lag effects on alignment but serializes producer and renderer; do not use it for timing. |
| `HAZE_BENCHMARK_MEMHASH=<path>` | Hash committed guest pages at gameplay frames 0–8 to locate diverging state between two runs. |
| `HAZE_BENCHMARK_KEEP_HISTORY=1` | Skip the gameplay-start frame-tick reset. |

Use `PAD_ALWAYS_CHANGE=1` with scripted pad input so every guest poll receives
a packet. A run normally also sets `HAZE_BENCHMARK_CAMERA_FILE` (the camera
audit path), loads a checkpoint save, waits for `<csv>.done`, and closes the
window so the fingerprint is written.

## Running the benchmark

```powershell
python scripts/diagnose.py timedemo-run <stem> --scene later [--exe build\haze\HazeRecomp.exe] [--shared-root <checkout>]
python scripts/diagnose.py timedemo-ab <tag> --control <exe> --candidate <exe> --scene later [--pairs 4]
```

`timedemo-run` starts the game on a private copy of a committed checkpoint,
loads it through the main menu, settles for `--settle` gameplay frames, runs
the camera sweep for `--sweep` frames and closes the game. Reports land in
`local/timedemo/out/<stem>-*`; the private game copy is deleted. It needs a
prepared checkout (`game/disc`, firmware) and an interactive desktop, and the
game window keeps focus. `timedemo-ab` runs ABBA pairs on a quiet host and
reports raw and draw-adjusted differences with an exact permutation test.

| Scene | Checkpoint |
| --- | --- |
| `later` | Later combat, the default |
| `deck` | The campaign's opening carrier deck |
| `mine` | Desert mine, vehicle start |

`config/benchmarks/scenes/<scene>.tar.xz` holds only the checkpoint's save
state (`PARAM.SFO`, `_PROFILE.DAT`, `CHCKPNTS.SAV`). The save's icons and
sound are copied from the local disc, and no shader cache is committed: the
first run of a scene on a machine records one under `local/timedemo/cache`
and is not a measurement. The graphics profile is
`config/benchmarks/timedemo.ini`. To add a scene, play to a checkpoint, then
pack those three files from the save's profile directory with
`mtime` 0; `tests/test_benchmark_scenes.py` checks the archive contents.

Menu navigation is timed in wall-clock seconds. An occasional session misses
it and never reaches gameplay; `timedemo-run` retries it once.

## Comparing runs

```powershell
python scripts/diagnose.py timedemo <runA> <runB> [--window 600]
```

Each prefix names `<prefix>-timeline.csv` and `<prefix>-fingerprint.csv`. The
report gives sweep frame times (producer boundary intervals), escapes, and the
per-window draw-count agreement with the first run.

## Determinism limits

Measured on the later-combat checkpoint (two runs of one binary):

- Gameplay frames are bit-identical only for the first 2–4 frames. Menus,
  loading and the post-load intro take a variable number of frames because
  loading completes on its own thread; characters enter gameplay with slightly
  different animation state, and the difference grows chaotically.
- Pinning the process to one CPU or draining the GPU at every boundary does
  not change that divergence point. Making it exact needs a quiescence barrier
  over all guest threads at each frame, not attempted.
- With the reset, per-window draw counts stay within about ±4% for ~30 s of
  game time; then the scenes can differ (different events fire).

So the mode removes host-speed effects from the scene, but runs still differ
by a few percent in workload: pair A/B runs, check the draw-count agreement,
and use offline replays for sub-percent effects.
