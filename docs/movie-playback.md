# Native movie playback

The Windows live renderer plays the supported BLUS30094 disc's PAMF streams using
MPEG-2 video and ATRAC3+ audio. Opening logos and the idle attract movie play
by default. Escape and the current Cross/confirm binding skip, including
keyboard, controller, and mouse confirm bindings. Videos have no host skip
prompt. A skip press is consumed through release before title/menu input resumes.
`[Startup] AutoSkipLogos=1` skips the Ubisoft and Free Radical videos plus the
initial Haze/legal splash and "Online play not rated by ESRB" notice.
`AutoSkipAttract=1` independently skips the idle attract movie. Neither policy
automatically skips campaign movies or the interactive title/Press Start screen.

The two static startup screens also accept Escape or confirm with automatic
skipping disabled. Each press advances the current slide. Their textures are
`misctex/loading/legal_eng` and `misctex/loading/esrb_eng` in `sc_splashscreen_us`
(including the supported SD and French rating variants). The build guards the
BLUS30094 splash update at `0x6407c0` and texture loader at `0x640440`. Only those
known startup textures qualify; demo outro slides use the same scene class and
retain their normal behavior. Skipping removes fade/hold delays after the
original texture-readiness check. Scene initialization, completion, cleanup,
save/profile setup, and game-data checks still run normally. Static slides never
inject guest buttons. Setting `AutoSkipLogos=0` retains the normal timed path.

The reader accepts disc-relative `x-cell-fs:///dev_bdvd/` URIs, without a movie
filename whitelist. The supplied disc has five standalone PAMF files:
`ubi.pam`, `ubi43.pam`, `frd.pam`, `frd43.pam`, and `attract.pam`. Campaign
scenes rendered by the game continue through the normal game renderer.
This does not claim full campaign completion or support for other discs/codecs.

## Build

`scripts/build.ps1` downloads and verifies the pinned FFmpeg 8.1 LGPL shared
development package in `port/movie-dependency.cmake`. For an offline build,
pass `-FFmpegRoot C:\path\to\unpacked-package`; it must contain `include`,
`lib`, and `bin`. The build copies avcodec, avutil, swscale, and swresample
DLLs next to the executable; no FFmpeg command-line process is needed at runtime.
Installation includes the package's LGPL license and documentation. The
[BtbN build source](https://github.com/BtbN/FFmpeg-Builds) identifies the build
recipe; the pinned archive identifies its corresponding FFmpeg revision.
The dependency uses a month-end archive (upstream retains these for two years),
not an expiring daily build. Refresh the pin before September 2028 or preserve
the verified package for offline builds.

## Playback and transitions

The PAMF reader checks the header extent and stream table and reads bounded
PES packets. Video packets go through FFmpeg's MPEG-2 parser and decoder,
including the delayed pictures at EOF. ATRAC3+ access units are reconstructed
across PES boundaries; the first audio track is converted to 48 kHz stereo.
The other track is not mixed over it. Initial audio preroll is trimmed against
the common presentation timeline. MPEG-2 color range and matrix metadata
control RGBA conversion.

The audio device's consumed queue position drives video scheduling. Video
uses a monotonic clock when no audio device is available, and for the tail
after audio drains. Decoding runs ahead with bounded queues; late pictures
are dropped rather than delaying audio. Pause freezes both outputs. Stop,
cancel, close, replacement, finalization, and shutdown join the decoder and
discard queued audio and video.

The window/FIFO thread presents the latest due frame and suppresses the
guest's black movie presentation. It also owns renderer transitions and
guest UI composition. The decoder never calls guest functions or touches
the renderer. SAIL stays running until EOF. Haze's audited movie-frame poll
is bridged to the same movie-ended flag that its guest-buffer EOF check
normally sets; no fabricated guest texture pointer is returned. The build
rejects a changed guest function until that bridge is audited again.

## Validation

`scripts/build.ps1 -Test` includes malformed/truncated PAMF parsing, SAIL
lifecycle, full decoding of every disc movie, and timed playback tests with
real decoding/audio output and a controlled presentation sink. Timed tests
mute output, verify natural EOF, pause/resume, skip latency, ownership,
replacement, and shutdown. The input regression covers rebound skip keys,
controller-only input, Escape, focus loss, and release before menu input resumes.
`scripts/test-input.ps1` also executes the actual patched splash update to check
resource waits, normal timing, manual and automatic skipping, register preservation,
and exclusion of other slides. Changed guest code fails the build's digest guard.

Startup validation on Windows/BLUS30094 (2026-10-14): the two static screens
took 12.9 s with normal presentation and 0.36–0.44 s with automatic skipping.
That removes about 12.5 s of static-screen delay. Roughly 2.8 s of other startup
work before the first movie remained in both cases. With video skipping included,
the first title-menu update arrived at about 5.6 s instead of 33 s. These are local
measurements, not a startup-time guarantee. Fresh local state and subsequent
executable-launcher runs retained the title/Press Start screen; launching from a
different working directory used the same input preference. Captures verified
both original static screens when skipping was disabled and the title afterward.
Live Steam-client launch and physical keyboard interaction were not verified:
Windows denied the test process foreground focus. Manual skip/release behavior
is covered by the mapper and guest-update tests above.

For a shorter rebuild, use the `HazeMovieDecodeTest`, `HazeMoviePlaybackTest`,
`HazeMovieSailTest`, and `HazeMoviePamfTest` CMake targets. DecodeTest accepts
one or more movie paths; PlaybackTest accepts the disc directory. Neither
test includes or redistributes game assets.

Disc validation on BLUS30094 1.02 (Windows, 2026-10-13):

| Stream | Decoded pictures | Video duration | Decoded stereo sample frames |
| --- | ---: | ---: | ---: |
| ubi.pam | 303 | 10.110 s | 489472 |
| ubi43.pam | 306 | 10.210 s | 495616 |
| frd.pam | 176 | 5.873 s | 286720 |
| frd43.pam | 209 | 6.974 s | 339968 |
| attract.pam | 3653 | 121.888 s | 5855232 |

All streams produced ordered video timestamps and continuous, nonzero audio.
Native runs displayed both opening logos and the attract movie through the
actual GPU swap chain. Natural EOF drained the logos at 10.147 s and 5.924 s,
and the full attract stream at 121.935 s. A separate guest Cross input stopped
attract at 11.350 s and returned to the title screen; Start then opened the
campaign menus. Diagnostic runs muted output while retaining the audio device
clock. These checks do not constitute a full campaign playthrough.
The retained monthly FFmpeg build reproduced every decoded image hash and
audio sample count. Timed playback also passed with audio-device initialization
deliberately disabled, exercising the monotonic fallback clock.
After another attract skip with the final package, a new campaign loaded and
rendered its opening in-engine scene and credits. The movie renderer had
released ownership and no further SAIL movie was requested during that scene.

`HAZE_MOVIE_CAPTURE=<existing directory>` writes a GPU readback of the 60th
presented frame of each movie. This is an opt-in diagnostic and adds a stall.
Ordinary playback performs no GPU readback for this diagnostic. Logs report
decoded/submitted frames, audio sample counts, EOF/stop times, and the number
of frames presented by the renderer. Use `scripts/run.ps1` and its regular
capture trigger to inspect the return to menus/gameplay separately.

Format references: [PAMF layouts in RPCS3](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellPamf.h),
[FFmpeg decoder send/receive contract](https://www.ffmpeg.org/doxygen/8.1/group__lavc__decoding.html).
