# Performance overlay

Video Settings offers **Off**, **Compact**, and **Expanded**, plus four corner
positions. Apply saves both preferences in `graphics.ini`. F7 cycles modes and
saves immediately, including during gameplay. F6 also reloads these settings:

```ini
[graphics]
performance_overlay = compact
overlay_position = top-left
```

`HAZE_PERFORMANCE_OVERLAY` and `HAZE_OVERLAY_POSITION` override the saved values.
F7 reports an active mode override in the window title instead of silently
overriding it. Failed saves retain the previous effective mode.

Compact is a small green FPS number with a black text shadow and no panel,
tucked into the corner. It targets roughly 11 physical pixels of text height;
very low rendering resolutions retain at least one render pixel per font dot
for readability when enlarged. Expanded uses a separate small opaque panel.
Neither mode has an input surface or changes mouse capture.

Expanded labels the following measurements:

| Field | Meaning |
| --- | --- |
| Present | Successful DXGI presentations per second, from the renderer's bounded QPC history |
| Interval | Mean interval between those successful presentations, including pacing and waits |
| Guest flips | Guest-frame counter advancement over the same window; explicitly **not simulation rate** |
| Draws | Renderer draw-packet counter for the last completed guest frame |
| Tex cache | Current renderer texture-cache entry count, not texture memory |
| Process RAM (WS) | Process working-set memory in MiB, not GPU memory |
| CPU / GPU time | N/A: the normal renderer does not expose reliable full-frame timings |

FPS and interval use the same recent-window boundary as the window-title FPS
counter (normally about one second; up to two seconds at low rates). Values
refresh at most four times per second and describe completed presents, so the
overlay and title can differ briefly during stalls or load transitions.

The renderer draws one triangle into the final backbuffer after postprocessing
and movie composition. The font and text use a fixed shader and 64 DWORDs of root
constants. There are no per-frame uploads, allocations, extra fences, timestamp
queries, or diagnostic file writes. Disabled drawing returns immediately.
Shaders are prepared at renderer startup, so F7 does not compile a shader.

## Validation

Run `powershell -ExecutionPolicy Bypass -File scripts/test-overlay.ps1` for
counter, layout, INI persistence, menu staging, transaction failure, input, and
reserved-key tests. Add `-Gpu` on Windows with D3D12 for a real shader/readback
test at 640x480, 1280x720, 2560x1080, and 3840x2160. It checks all four corners,
pixel bounds, transparent compact rendering, and the disabled no-op, and reports
CPU recording and GPU timestamp costs. Queries/readbacks exist only in this test.

Local acceptance on 2026-10-14 included a native build, title/menu and jungle
gameplay, 30/60 FPS caps, live F7/F6 changes, preference retention across restart,
and windowed, borderless, and active DXGI exclusive fullscreen. Compact was
reduced to the small single-line counter after the initial gameplay comparison.
The final real-device test measured approximately 6–14 microseconds of CPU work
even when forcing a stats refresh on every draw, and under 0.008 ms of GPU time
through 4K. The disabled call measured about 0.03–0.05 microseconds.

Six alternating 15-second gameplay samples at a 60 FPS cap had 95th-percentile
presentation intervals of 17.26–17.35 ms across Off, Compact, and Expanded. Most
blocks averaged 58.0–58.5 FPS. One early Expanded block included a 335.5 ms stall
and averaged 57.2 FPS; a later Expanded block averaged 58.5 FPS with a 21.8 ms
maximum. These whole-game samples include scheduling/game variation and do not
attribute that isolated stall to the overlay. The isolated GPU test measures the
overlay's own work separately. Results are local measurements, not a guarantee
for every GPU or driver.
