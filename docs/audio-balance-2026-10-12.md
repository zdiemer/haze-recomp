# Audio balance and DSP timing (#24)

This change starts from main's SDK revision `8660186`. It corrects stereo
routing and accelerates the SPU arithmetic used heavily by Haze's audio DSP.

## Routing

The host backends all submit stereo, but device queries previously advertised
2, 6 and 8 channels. Haze selected eight-channel output with no downmixer.
The host then added center, LFE and surround channels with unrelated gains,
which could turn guest samples below full scale into clipped output. One
captured block peaked at 0.851 in the guest and 1.353 after the old host mix.

Queries now advertise stereo only. Haze requests type-A downmix, stored at
byte 12 of the 16-byte guest configuration. The mixer uses 0.707 for front L/R,
0.5 for center and each surround channel, and excludes LFE. NONE/type-B routing
retains the front pair on this stereo device. Configuration/state queries
round-trip the selected mode. These coefficients and channel routing follow
[RPCS3 cellAudio](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellAudio.cpp);
the structure layout follows
[cellAudioOut.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellAudioOut.h).

## Producer cost

A native CPU sample located substantial audio-worker time in the exact integer
SPU float helpers. The optional Windows/Clang kernels use AVX-512 embedded
round-toward-zero arithmetic without changing MXCSR. Extended-range and
potential-overflow lanes retain the scalar implementation. CPU and OS vector
state checks gate startup; unsupported machines retain scalar arithmetic.
Haze enables the kernels by default where available. Set `SPU_FLOAT_NATIVE=0`
to compare the scalar path, or build with `PS3RECOMP_NATIVE_FLOAT_RUNTIME=OFF`.
The SDK itself defaults the build option off.

The vector calling convention avoids Windows union argument/return spills.
The measured multiply-add microbenchmark dropped from roughly 100?125 cycles
per four-lane operation to 18?20 on this machine. This is a kernel measurement,
not an end-to-end frame-rate claim. `SPU_FLOAT_NATIVE_VALIDATE=1` compares
results to the scalar implementation and latches the scalar path after any
mismatch; it is intentionally expensive.

## Validation and limits

- Impulse routing tests exercise all eight channels under NONE, type A and
  type B, alongside existing ring wrap/clearing/tag tests.
- Guest ABI tests cover stereo availability, configuration offset/size,
  big-endian round trips, invalid modes and boundary canaries.
- 73,679,652 native/reference lane comparisons cover edge and random operands,
  four host rounding modes with DAZ/FTZ enabled and disabled, and unchanged
  MXCSR. An injected mismatch verifies scalar substitution and disablement.
- Gameplay used an isolated copy of the real save, at the jungle checkpoint
  after dismissing the Nectar tutorial, with 48-kHz stereo float capture.
  The final instrumented run (`20261013-064241-734`) measured 12.04 seconds of
  ambience with peak 0.168, RMS 0.0281, no clipped samples and no all-zero blocks.
  A 12.19-second firing interval measured peak 0.991, RMS 0.0747, no clipped
  samples and 2.80% all-zero 256-frame blocks. DMA tracing over the last
  24 seconds measured 187.58 writes/second versus the required 187.5.
- Earlier same-process scalar/native comparisons reduced zero-block incidence
  substantially (20.77% to 1.83% in one pair of approximately 12-second windows),
  but gameplay and scheduling varied. Zero blocks are a diagnostic, not proof
  that every instance is an underrun. Other user-owned workloads were running.

The captures demonstrate corrected routing and reduced DSP cost. They do not
establish that all combat stuttering is eliminated, that every effect is
subjectively balanced, or that later missions are validated. Keep #24 open for
listening and broader gameplay verification. Unsupported CPUs receive the
routing correction but not this arithmetic speedup. Temporary DMA/raw-port
instrumentation, captures and save copies are excluded from the committed code.
