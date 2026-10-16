param(
    [string]$SharedRoot,
    [string]$SdkRoot,
    [string]$SDL3Root,
    [string]$FFmpegRoot,
    [string]$Python,
    [switch]$RuntimeOnly,
    [switch]$Test,
    [switch]$ProfileRenderer,
    [switch]$ProfileJobs,
    [switch]$ExperimentalInlineReads,
    [switch]$CompareSpuByteOps,
    [switch]$SpuFma,
    [switch]$NativeSpuFloat = $true,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [string]$OutputDirectory,
    [int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
if ($CompareSpuByteOps -and (-not $ProfileRenderer -or $RuntimeOnly)) {
    throw 'CompareSpuByteOps requires a full ProfileRenderer build.'
}
$root = Split-Path $PSScriptRoot -Parent
$assets = if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
Set-Location -LiteralPath $root
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# Import the compiler/SDK environment into this process. No filesystem
# operations are delegated to cmd.exe.
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio environment.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$clang = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
if ($env:HAZE_LLVM_ROOT) { $clang = Join-Path $env:HAZE_LLVM_ROOT 'bin/clang-cl.exe' }
if (-not $Python) { $Python = Join-Path $assets '.venv\Scripts\python.exe' }
$sdk = if ($SdkRoot) { (Resolve-Path -LiteralPath $SdkRoot).Path } elseif (Test-Path -LiteralPath "$root/external/ps3recomp/CMakeLists.txt") { Join-Path $root 'external\ps3recomp' } else { Join-Path $assets 'external\ps3recomp' }
if ($RuntimeOnly) {
    $source = $sdk
    $build = Join-Path $root 'build\runtime'
} else {
    if (-not (Test-Path -LiteralPath "$assets/generated/ppu/ppu_recomp.h")) {
        throw 'No Haze lift exists. Run .venv\Scripts\python.exe scripts/haze.py lift first.'
    }
    $source = Join-Path $root 'port'
    $build = Join-Path $root 'build\haze'
}
$ErrorActionPreference = 'Continue' # Windows PowerShell treats native stderr warnings as errors.
$exeOutput = if ($OutputDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory) } else { $build }
$controllerArgs = @()
if (-not $RuntimeOnly) { $controllerArgs += "-DHAZE_EXPERIMENT_INLINE_READS=$([int][bool]$ExperimentalInlineReads)" }
if ($SDL3Root) { $controllerArgs += "-DFETCHCONTENT_SOURCE_DIR_SDL3=$((Resolve-Path -LiteralPath $SDL3Root).Path)" }
if ($FFmpegRoot) { $controllerArgs += "-DHAZE_FFMPEG_ROOT=$((Resolve-Path -LiteralPath $FFmpegRoot).Path)" }
& $cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$exeOutput" "-DPython3_EXECUTABLE=$python" "-DPS3RECOMP_DIR=$sdk" "-DHAZE_SHARED_ROOT=$assets" "-DRECOMP_DIR=$assets/generated/ppu" "-DSPU_DIR=$assets/generated/spu" "-DHAZE_RSX_PROFILE=$([int][bool]$ProfileRenderer)" "-DHAZE_JOB_DIAGNOSTICS=$([int][bool]$ProfileJobs)" "-DHAZE_SPU_FMA=$([int][bool]$SpuFma)" "-DHAZE_SPU_BYTE_COMPARE=$([int][bool]$CompareSpuByteOps)" "-DPS3RECOMP_NATIVE_FLOAT_RUNTIME=$([int][bool]$NativeSpuFloat)" @controllerArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
if (-not $RuntimeOnly) {
    & $python "$root/tests/check_profile_configuration.py" $build --enabled $([int][bool]$ProfileRenderer) --jobs-enabled $([int][bool]$ProfileJobs) --inline-reads $([int][bool]$ExperimentalInlineReads)
    if ($LASTEXITCODE -ne 0) { throw 'Profiling definitions do not match the requested build.' }
}
& $cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
if ($Test) {
    & $python "$root/tests/test_profile_configuration.py"
    if ($LASTEXITCODE -ne 0) { throw 'Renderer/job profiling gate regression failed.' }
    & $python "$root/tests/test_milestone_async.py" --compiler $clang --sdk $sdk --output-dir "$root/local"
    if ($LASTEXITCODE -ne 0) { throw 'Asynchronous milestone writer regression failed.' }
    & $python "$sdk/tools/test_ppu_jumptables.py"
    if ($LASTEXITCODE -ne 0) { throw 'PPU local jump-table discovery regression failed.' }
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_local_dispatch.py" --compiler $clang --source "$assets/generated/ppu/ppu_recomp_000.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Local jump-table dispatch and shared-frame regression failed.' }
    }
    & $clang /nologo /W0 /O2 /std:c++17 /EHsc "/Fe$root/local/test_spu_idle_workers.exe" "/Fo$root/local/test_spu_idle_workers.obj" "$root/tests/test_spu_idle_workers.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'SPU idle-worker cache compilation failed.' }
    & "$root/local/test_spu_idle_workers.exe"
    if ($LASTEXITCODE -ne 0) { throw 'SPU idle-worker cache regression failed.' }
    & $clang /nologo /W0 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS "/I$sdk/libs/video" "/Fe$root/local/test_rsx_profile_buffer.exe" "/Fo$root/local/test_rsx_profile_buffer.obj" "$sdk/libs/video/tests/test_rsx_profile_buffer.c"
    if ($LASTEXITCODE -ne 0) { throw 'Deferred profile buffer compilation failed.' }
    & "$root/local/test_rsx_profile_buffer.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Deferred profile buffer regression failed.' }
    & $clang /nologo /W0 /O2 /std:c11 /Gy /Gw "/I$sdk/include" "/I$sdk/libs/video" "/Fe$root/local/test_rsx_vp_burst.exe" "/Fo$root/local/" "$sdk/libs/video/tests/test_rsx_vp_burst.c" "$sdk/libs/video/rsx_vp_burst.c" "$sdk/libs/video/rsx_dispatch.c" "$sdk/libs/video/rsx_commands.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'Native VP consumer kernel compilation failed.' }
    & "$root/local/test_rsx_vp_burst.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Native VP consumer state parity failed.' }
    & $python "$root/tests/test_vp_consumer_boundary.py" --compiler $clang --sdk $sdk --output-dir "$root/build/vp-consumer-boundary"
    if ($LASTEXITCODE -ne 0) { throw 'Native VP consumer FIFO/wrapper/diagnostic boundary tests failed.' }
    & $python "$root/tests/test_fifo_slow_drain.py" --compiler $clang --sdk $sdk --output-dir "$root/build/fifo-slow-drain"
    if ($LASTEXITCODE -ne 0) { throw 'FIFO slow-drain lock/body diagnostic regression failed.' }
    & $python "$sdk/libs/video/tests/test_gcm_location_resolution.py" --compiler $clang
    if ($LASTEXITCODE -ne 0) { throw 'Known-location GCM address resolution regression failed.' }
    & $python "$sdk/runtime/spu/tests/test_signal_wake.py" --compiler $clang
    if ($LASTEXITCODE -ne 0) { throw 'SPU atomic signal wake/lifetime regression failed.' }
    & $python "$sdk/libs/spurs/tests/test_taskset_guest_fields.py" --compiler $clang --output-dir "$root/build/taskset-guest-fields"
    if ($LASTEXITCODE -ne 0) { throw 'Taskset shutdown/destroy guest-memory regression failed.' }
    & $python "$sdk/tools/test_spu_interpreter_ilh.py" --compiler $clang --output-dir "$root/build/spu-interpreter-upstream"
    if ($LASTEXITCODE -ne 0) { throw 'SPU interpreter ILH replication regression failed.' }
    & $python "$sdk/tools/test_spu_interpreter_ri10.py" --compiler $clang --output-dir "$root/build/spu-interpreter-upstream"
    if ($LASTEXITCODE -ne 0) { throw 'SPU interpreter RI10 byte/halfword regression failed.' }
    & $python "$sdk/tools/test_spu_disasm_ri10.py"
    if ($LASTEXITCODE -ne 0) { throw 'SPU RI10 decoder/table generation regression failed.' }
    & $python "$sdk/tools/test_spu_decrementer.py" --compiler $clang --output-dir "$root/build/spu-decrementer"
    if ($LASTEXITCODE -ne 0) { throw 'SPU decrementer overflow regression failed.' }
    & $python "$sdk/tools/test_ppu_fallthrough.py"
    if ($LASTEXITCODE -ne 0) { throw 'PPU syscall fall-through regression failed.' }
    & $python "$root/tests/test_fifo_recycle_trace.py" --compiler $clang --sdk $sdk --output-dir "$root/build/fifo-recycle-trace"
    if ($LASTEXITCODE -ne 0) { throw 'FIFO recycle acknowledgement diagnostic regression failed.' }
    & $python -m unittest tests.test_analyze_fifo_recycles
    if ($LASTEXITCODE -ne 0) { throw 'FIFO recycle QPC attribution regression failed.' }
    & $python "$root/tests/test_rsx_slow_flush.py" --compiler $clang --sdk $sdk --output-dir "$root/build/rsx-slow-flush"
    if ($LASTEXITCODE -ne 0) { throw 'RSX slow-flush phase and failure diagnostic regression failed.' }
    & $python "$root/tests/test_rsx_slow_draw_ordinary.py" --compiler $clang --source "$sdk/libs/video/rsx_live_draw.c" --output-dir "$root/build/rsx-slow-draw-ordinary"
    if ($LASTEXITCODE -ne 0) { throw 'Ordinary/profile slow-draw diagnostic regression failed.' }
    & $python "$root/tests/test_texture_diagnostic_defer.py" --compiler $clang --source "$sdk/libs/video/rsx_live_draw.c" --output-dir "$root/build/texture-diagnostic-defer"
    if ($LASTEXITCODE -ne 0) { throw 'Deferred texture diagnostic regression failed.' }
    & $python "$root/tests/test_vp_burst_reporting.py" --compiler $clang --sdk $sdk --output-dir "$root/build/vp-burst-reporting"
    if ($LASTEXITCODE -ne 0) { throw 'VP burst reporting regression failed.' }
    & $python "$root/tests/test_shader_cache_bundle.py" --compiler $clang --source "$sdk/libs/video/rsx_live_draw.c" --output-dir "$root/build/shader-cache-bundle"
    if ($LASTEXITCODE -ne 0) { throw 'Shader cache bundle regression failed.' }
    $previousBundleCompiler=$env:CLANG_CL
    try {
        $env:CLANG_CL=$clang
        & $python "$sdk/libs/video/tests/test_shader_bundle_shutdown.py"
        if ($LASTEXITCODE -ne 0) { throw 'Terminal shader bundle ownership regression failed.' }
    } finally { $env:CLANG_CL=$previousBundleCompiler }
    & $python "$root/tests/test_analyze_slow_drains.py"
    if ($LASTEXITCODE -ne 0) { throw 'Slow-drain/flush QPC attribution regression failed.' }
    & $python "$root/tests/test_pad_file_detach.py" --compiler $clang --sdk $sdk --output-dir "$root/build/pad-file-detach"
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic controller detach regression failed.' }
    New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_movie_patch.py" "$assets/generated/ppu/ppu_recomp_003.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Movie guest EOF patch regression failed.' }
        & $python "$root/tests/test_startup_splash_patch.py" "$assets/generated/ppu/ppu_recomp_003.cpp" "$clang" "$sdk"
        if ($LASTEXITCODE -ne 0) { throw 'Startup splash transition regression failed.' }
        & $cmake --build $build --target HazeMovieDecodeTest HazeMoviePlaybackTest HazeMovieSailTest HazeMoviePamfTest --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'Movie test compilation failed.' }
        & "$exeOutput/HazeMovieSailTest.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Movie lifecycle regression failed.' }
        & "$exeOutput/HazeMoviePamfTest.exe"
        if ($LASTEXITCODE -ne 0) { throw 'PAMF parsing regression failed.' }
        $movies = Get-ChildItem -LiteralPath "$assets/game/disc/PS3_GAME/USRDIR/assets/g5/pamf" -Filter '*.pam'
        if (-not $movies) { throw 'Supported-disc movies are required for movie validation.' }
        & "$exeOutput/HazeMovieDecodeTest.exe" @($movies.FullName)
        if ($LASTEXITCODE -ne 0) { throw 'Supported-disc movie decoding failed.' }
        & "$exeOutput/HazeMoviePlaybackTest.exe" "$assets/game/disc"
        if ($LASTEXITCODE -ne 0) { throw 'Timed playback/skip/pause regression failed.' }
    }
    & $python "$root/tests/test_memory_read_fastpaths.py" --compiler $clang --sdk $sdk --output-dir "$root/build/memory-read-fastpaths"
    if ($LASTEXITCODE -ne 0) { throw 'Native byte/halfword memory-read regression failed.' }
    & $python "$root/tests/test_ppu_coherency_predicate.py" --compiler $clang --sdk $sdk --output-dir "$root/build/ppu-coherency-predicate"
    if ($LASTEXITCODE -ne 0) { throw 'PPU coherency predicate startup/store regression failed.' }
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_ppu_inline_reads.py" --compiler $clang --sdk $sdk --recomp "$assets/generated/ppu" --output-dir "$root/build/ppu-inline-reads"
        if ($LASTEXITCODE -ne 0) { throw 'Generated inline memory-read regression failed.' }
    }
    $audioRingTest = Join-Path $root 'local/test_audio_ring.exe'
    & $clang /nologo /W0 /O2 /std:c11 /Gy /Gw "/I$sdk/include" "/Fe$audioRingTest" "/Fo$root/local/test_audio_ring.obj" "$sdk/libs/audio/tests/test_audio_ring.c" /link /OPT:REF ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Audio ring/routing test compilation failed.' }
    & $audioRingTest
    if ($LASTEXITCODE -ne 0) { throw 'Audio ring/routing regression failed.' }
    & $python "$root/tests/test_wait_timeline.py"
    if ($LASTEXITCODE -ne 0) { throw 'Wait timeline syscall naming and interval-union tests failed.' }
    & $python "$root/tests/test_camera_sweep.py"
    if ($LASTEXITCODE -ne 0) { throw 'Camera benchmark trajectory evidence tests failed.' }
    & $python "$root/tests/test_camera_benchmark_io.py" --compiler $clang --output-dir "$root/build/camera-benchmark-io"
    if ($LASTEXITCODE -ne 0) { throw 'Camera benchmark asynchronous control and deferred audit tests failed.' }
    $graphicsTest = Join-Path $root 'local/test_haze_graphics.exe'
    & $clang /nologo /W0 /O2 /std:c++20 /EHsc "/Fe$root/local/test_haze_overlay.exe" "/Fo$root/local/test_haze_overlay.obj" "$root/tests/test_haze_overlay.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Performance overlay test compilation failed.' }
    & "$root/local/test_haze_overlay.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Performance overlay counter/layout regression failed.' }
    & $clang /nologo /W0 /O1 /std:c++20 "/I$sdk/libs/video" "/Fe$graphicsTest" "/Fo$root/local/test_haze_graphics.obj" "$root/tests/test_haze_graphics.cpp" /link user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Graphics configuration test compilation failed.' }
    & $graphicsTest
    if ($LASTEXITCODE -ne 0) { throw 'Graphics configuration test failed.' }
    $pacerSettingsTest = Join-Path $root 'local/test_frame_pacer_settings.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/Fe$pacerSettingsTest" "/Fo$root/local/test_frame_pacer_settings.obj" "$root/tests/test_frame_pacer_settings.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Native frame-pacer selection test compilation failed.' }
    & $pacerSettingsTest
    if ($LASTEXITCODE -ne 0) { throw 'Native frame-pacer selection test failed.' }
    $displayTest = Join-Path $root 'local/test_haze_display.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/I$sdk/libs/video" "/Fe$displayTest" "/Fo$root/local/test_haze_display.obj" "$root/tests/test_haze_display.cpp" /link user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Display-window test compilation failed.' }
    & $displayTest
    if ($LASTEXITCODE -ne 0) { throw 'Display-window test failed.' }
    $bytesTest = Join-Path $root 'local/test_spu_bytes.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$bytesTest" "/Fo$root/local/test_spu_bytes.obj" "$sdk/runtime/spu/tests/test_spu_bytes.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU SIMD byte-operation test compilation failed.' }
    & $bytesTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU SIMD byte-operation oracle test failed.' }
    $fiTest = Join-Path $root 'local/test_fi_scaling.exe'
    $fiFlags = @('/clang:-msse4.1')
    if ($SpuFma) { $fiFlags += @('/clang:-mavx', '/clang:-mfma') }
    & $clang /nologo /W0 /O2 @fiFlags "/I$sdk/include" "/I$sdk/runtime/spu" "/Fe$fiTest" "/Fo$root/local/test_fi_scaling.obj" "$sdk/runtime/spu/tests/test_fi_scaling.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU FI scaling oracle compilation failed.' }
    & $fiTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU FI scaling rounding/denormal oracle failed.' }
    $numericTest = Join-Path $root 'local/test_spu_numeric_semantics.exe'
    & $clang /nologo /W0 /O2 @fiFlags "/I$sdk/include" "/I$sdk/runtime/spu" "/Fe$numericTest" "/Fo$root/local/test_spu_numeric_semantics.obj" "$sdk/runtime/spu/tests/test_spu_numeric_semantics.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU numeric semantics test compilation failed.' }
    & $numericTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU numeric/estimate-refinement test failed.' }
    if ($NativeSpuFloat) {
        $nativeFloatTest = Join-Path $root 'local/test_spu_float_native.exe'
        & $clang /nologo /W0 /O2 /Ob1 /clang:-O2 /clang:-msse4.1 /clang:-ffp-contract=off /DSPU_FLOAT_NATIVE_RUNTIME=1 "/I$sdk/runtime/spu" "/Fe$nativeFloatTest" "/Fo$root/local/test_spu_float_native.obj" "$sdk/runtime/spu/tests/test_spu_float_native.c"
        if ($LASTEXITCODE -ne 0) { throw 'Native SPU floating-point oracle compilation failed.' }
        & $nativeFloatTest
        if ($LASTEXITCODE -ne 0) { throw 'Native SPU hardware arithmetic/FP state/fallback oracle failed.' }
    }
    $coherencyTest = Join-Path $root 'local/test_coherency_lifetime.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$coherencyTest" "/Fo$root/local/" "$sdk/runtime/spu/tests/test_coherency_lifetime.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency lifetime test compilation failed.' }
    & $coherencyTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency lifetime/holder-capacity test failed.' }
    $coherencyStoreTest = Join-Path $root 'local/test_spu_coherency.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$coherencyStoreTest" "/Fo$root/local/test_spu_coherency.obj" "$sdk/runtime/spu/tests/test_spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency store-path test compilation failed.' }
    & $coherencyStoreTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency store/notification regression failed.' }
    $coherencyAtomicTest = Join-Path $root 'local/test_coherency_atomic_visibility.exe'
    & $clang /nologo /W0 /O2 /EHsc /std:c++17 /clang:-msse4.1 "/I$sdk/include" "/Fe$coherencyAtomicTest" "/Fo$root/local/" "$sdk/runtime/spu/tests/test_coherency_atomic_visibility.cpp" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU reservation publication test compilation failed.' }
    & $coherencyAtomicTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU reservation publication/polling regression failed.' }
    $coherencyLockTest = Join-Path $root 'local/test_coherency_lock.exe'
    & $clang /nologo /W0 /O2 /std:c11 /clang:-msse4.1 "/I$sdk/include" "/Fe$coherencyLockTest" "/Fo$root/local/" "$sdk/runtime/spu/tests/test_coherency_lock.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherence lock test compilation failed.' }
    & $coherencyLockTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherence lock exclusion/publication regression failed.' }
    $idleTest = Join-Path $root 'local/test_spu_idle_wait.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/runtime/spu" "/Fe$idleTest" "/Fo$root/local/" "$root/tests/test_spu_idle_wait.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'Haze SPU idle wait test compilation failed.' }
    & $idleTest
    if ($LASTEXITCODE -ne 0) { throw 'Haze SPU idle wait predicate/publication test failed.' }
    if (-not $RuntimeOnly) {
        $tripletOracle = Join-Path $root 'local/triplet_oracle.c'
        & $python "$root/scripts/patch.py" oracle-triplet "$assets/generated/spu/spu_0041_at_00F2E380/spu_recomp.c" $tripletOracle
        if ($LASTEXITCODE -ne 0) { throw 'Audited triplet-copy oracle extraction failed.' }
        $tripletTest = Join-Path $root 'local/test_spu_triplet_copy.exe'
        & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/runtime/spu" "/I$root/port" "/Fe$tripletTest" "/Fo$root/local/" "$root/tests/test_spu_triplet_copy.c" $tripletOracle
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy oracle compilation failed.' }
        & $tripletTest
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy full-state oracle failed.' }
        & $python "$root/tests/test_spu_triplet_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy patch guards failed.' }
        $cursorOracle = Join-Path $root 'local/cursor_oracle.c'
        & $python "$root/scripts/patch.py" oracle-cursor "$assets/generated/spu/spu_0039_at_00F1C100/spu_recomp.c" $cursorOracle
        if ($LASTEXITCODE -ne 0) { throw 'Native cursor oracle extraction failed.' }
        $cursorTest = Join-Path $root 'local/test_native_cursor.exe'
        & $clang /nologo /W0 /O2 @fiFlags "/I$sdk/runtime/spu" "/I$root/port" "/Fe$cursorTest" "/Fo$root/local/" "$root/tests/test_native_cursor.c" $cursorOracle
        if ($LASTEXITCODE -ne 0) { throw 'Native cursor oracle compilation failed.' }
        & $cursorTest
        if ($LASTEXITCODE -ne 0) { throw 'Native cursor boundary oracle failed.' }
        & $python "$root/tests/test_native_cursor_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Native cursor patch guard tests failed.' }
        $job0Oracle = Join-Path $root 'local/job0_oracle.c'
        & $python "$root/scripts/patch.py" oracle-job0 "$assets/generated/spu/spu_0029_at_00EC9A80/spu_recomp.c" $job0Oracle
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 compute oracle extraction failed.' }
        $job0Flags = @('/DSPU_FLOAT_NATIVE_RUNTIME=1', '/clang:-ffp-contract=off') + $fiFlags
        & $clang /nologo /W0 /O2 /Ob1 @job0Flags "/I$sdk/runtime/spu" "/I$root/port" "/Fe$root/local/test_job0_compute.exe" "/Fo$root/local/" "$root/tests/test_job0_compute.c" $job0Oracle "$sdk/runtime/spu/spu_float_native.c"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 compute oracle compilation failed.' }
        & "$root/local/test_job0_compute.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 full-state compute oracle failed.' }
        & $clang /nologo /W0 /O2 /Ob1 @job0Flags "/I$sdk/runtime/spu" "/I$root/port" "/Fe$root/local/test_job0_arithmetic.exe" "/Fo$root/local/" "$root/tests/test_job0_arithmetic.c" "$sdk/runtime/spu/spu_float_native.c"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 arithmetic oracle compilation failed.' }
        & "$root/local/test_job0_arithmetic.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 arithmetic oracle failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++20 /DHAZE_JOB_TRACE "/Fe$root/local/test_job0_capture.exe" "/Fo$root/local/" "$root/tests/test_job0_capture.cpp" "$root/port/haze_job0_capture.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 fixture recorder compilation failed.' }
        & "$root/local/test_job0_capture.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 fixture recorder test failed.' }
        & $python "$root/tests/test_job0_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job-0 patch guard tests failed.' }
    }
    $abiStorageTest = Join-Path $root 'local/test_abi_storage.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$abiStorageTest" "/Fo$root/local/test_abi_storage.obj" "$sdk/runtime/spu/tests/test_abi_storage.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU ABI storage test compilation failed.' }
    & $abiStorageTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU ABI storage semantics test failed.' }
    $depthTest = Join-Path $root 'local/test_depth_history.exe'
    & $clang /nologo /W0 /O2 /Gy /Gw "/I$sdk/include" "/Fe$depthTest" "/Fo$root/local/test_depth_history.obj" "$sdk/runtime/spu/tests/test_depth_history.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'SPU private-history test compilation failed.' }
    & $depthTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU history isolation/depth-limit test failed.' }
    $drainHooksTest = Join-Path $root 'local/test_drain_hooks.exe'
    & $clang /nologo /W0 /O2 /Gy /Gw "/I$sdk/include" "/Fe$drainHooksTest" "/Fo$root/local/test_drain_hooks.obj" "$sdk/runtime/spu/tests/test_drain_hooks.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'SPU drain hook test compilation failed.' }
    foreach ($mode in @('default', 'no-ls0', 'exit', 'step', 'jobdrain')) {
        & $drainHooksTest $mode
        if ($LASTEXITCODE -ne 0) { throw "SPU drain hook test failed: $mode" }
    }
    $sleepTest = Join-Path $root 'local/test_usleep_timer.exe'
    & $clang /nologo /W0 /O2 "/Fe$sleepTest" "/Fo$root/local/test_usleep_timer.obj" "$sdk/runtime/syscalls/tests/test_usleep_timer.c"
    if ($LASTEXITCODE -ne 0) { throw 'High-resolution timer test compilation failed.' }
    & $sleepTest
    if ($LASTEXITCODE -ne 0) { throw 'High-resolution timer duration/handle test failed.' }
    $fingerprintTest = Join-Path $root 'local/test_fingerprint_cache.exe'
    & $clang /nologo /W0 /O2 "/I$sdk/include" "/Fe$fingerprintTest" "/Fo$root/local/test_fingerprint_cache.obj" "$sdk/runtime/spu/tests/test_fingerprint_cache.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU fingerprint cache test compilation failed.' }
    & $fingerprintTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU fingerprint cache identity/concurrency test failed.' }
    foreach ($primitive in @('test_spu_helpers', 'test_spu_shufb')) {
        $primitiveTest = Join-Path $root "local/$primitive.exe"
        & $clang /nologo /W0 /O1 /clang:-msse4.1 "/I$sdk/include" "/Fe$primitiveTest" "/Fo$root/local/$primitive.obj" "$sdk/runtime/spu/tests/$primitive.c"
        if ($LASTEXITCODE -ne 0) { throw "SPU SIMD test compilation failed: $primitive" }
        & $primitiveTest
        if ($LASTEXITCODE -ne 0) { throw "SPU SIMD execution failed: $primitive" }
    }
    $fmaTest = Join-Path $root 'local/test_spu_fma.exe'
    $fmaFlags = $(if ($SpuFma) { @('/clang:-mavx', '/clang:-mfma') } else { @() })
    & $clang /nologo /W0 /O2 /clang:-msse4.1 @fmaFlags "/I$sdk/include" "/Fe$fmaTest" "/Fo$root/local/test_spu_fma.obj" "$root/tests/test_spu_fma.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU fused arithmetic test compilation failed.' }
    $floatVectors = Join-Path $root 'local/spu-float-oracle.bin'
    & $python "$sdk/runtime/spu/tests/test_spu_float_oracle.py" $floatVectors
    if ($LASTEXITCODE -ne 0) { throw 'SPU exact arithmetic oracle generation failed.' }
    & $fmaTest $floatVectors
    if ($LASTEXITCODE -ne 0) { throw 'SPU extended single-precision arithmetic regression failed.' }
    $frameTest = Join-Path $root 'local/test_frame_pump.exe'
    $pumpTest = Join-Path $root 'local/test_pump_intervals.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/Fe$pumpTest" "/Fo$root/local/test_pump_intervals.obj" "$root/tests/test_pump_intervals.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Pump interval test compilation failed.' }
    & $pumpTest
    if ($LASTEXITCODE -ne 0) { throw 'Pump interval accounting test failed.' }
    & $python "$root/tests/test_pump_summary.py"
    if ($LASTEXITCODE -ne 0) { throw 'Pump interval summary test failed.' }
    & $python "$root/tests/test_pump_waits.py"
    if ($LASTEXITCODE -ne 0) { throw 'Pump/PPU wait correlation test failed.' }
    & $clang /nologo /W0 /O1 /std:c++20 "/Fe$frameTest" "/Fo$root/local/test_frame_pump.obj" "$root/tests/test_frame_pump.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Frame-pump test compilation failed.' }
    & $frameTest
    if ($LASTEXITCODE -ne 0) { throw 'Frame-boundary presentation test failed.' }
    & $python "$root/tests/test_firmware_relocation.py"
    if ($LASTEXITCODE -ne 0) { throw 'Firmware ELF relocation tests failed.' }
    & $python "$root/tests/test_present_modes.py"
    if ($LASTEXITCODE -ne 0) { throw 'Present-window benchmark validation failed.' }
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_spu_job_modes.py"
        if ($LASTEXITCODE -ne 0) { throw 'SPU job mode-summary validation failed.' }
        & $python "$root/tests/test_guest_pacer_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Guest pacing patch guard validation failed.' }
        & $python "$root/tests/test_job_wait_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job completion patch guard validation failed.' }
        & $python "$root/tests/test_job_dispatch_trace_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job trace boundary guard validation failed.' }
        & $python "$root/tests/test_job_trace_analysis.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job trace analysis regression failed.' }
        & $python "$root/tests/test_job_wait_frames.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job trace interval accounting failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++20 /DHAZE_JOB_TRACE /DHAZE_JOB_TRACE_TEST_CAPACITY=64 "/Fe$root/local/test_job_trace.exe" "/Fo$root/local/" "$root/tests/test_job_trace.cpp" "$root/port/haze_job_trace.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Job trace test compilation failed.' }
        & "$root/local/test_job_trace.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Job trace concurrency and retention tests failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++20 "/Fe$root/local/test_job_trace_noop.exe" "/Fo$root/local/" "$root/tests/test_job_trace_noop.cpp" "$root/port/haze_job_trace.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Ordinary job trace no-op compilation failed.' }
        & "$root/local/test_job_trace_noop.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Ordinary job trace no-op test failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++20 "/Fe$root/local/test_job_profile.exe" "/Fo$root/local/test_job_profile.obj" "$root/tests/test_job_profile.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Job completion profiler test compilation failed.' }
        & "$root/local/test_job_profile.exe" "$root/local/test-job-profile.csv"
        if ($LASTEXITCODE -ne 0) { throw 'Job completion profiler accounting failed.' }
        & $python "$root/tests/test_job_completion_summary.py"
        if ($LASTEXITCODE -ne 0) { throw 'Job completion summary validation failed.' }
        & $clang /nologo /EHsc /W0 /O2 "/I$assets/generated/ppu" "/Fe$root/local/test_job_wait.exe" "/Fo$root/local/" "$root/tests/test_job_wait.cpp" "$root/port/haze_job_wait.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Native job wait test compilation failed.' }
        & "$root/local/test_job_wait.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native job wait scope validation failed.' }
        & $python "$root/tests/test_bulk_fill.py" --compiler $clang --sdk $sdk --generated "$assets/generated/ppu" --output-dir "$root/build/bulk-fill-tests"
        if ($LASTEXITCODE -ne 0) { throw 'Native slot-fill source/boundary differential regression failed.' }
        & $python "$root/tests/test_slot_copy.py" --compiler $clang --sdk $sdk --generated "$assets/generated/ppu" --output-dir "$root/build/slot-copy-tests"
        if ($LASTEXITCODE -ne 0) { throw 'Native slot-copy source/boundary differential regression failed.' }
        & $python -m unittest tests.test_producer_profile_patch
        if ($LASTEXITCODE -ne 0) { throw 'Main producer phase patch preservation failed.' }
        & $python -m unittest tests.test_analyze_producer_profile
        if ($LASTEXITCODE -ne 0) { throw 'Main producer phase analysis validation failed.' }
        & $python "$root/tests/test_producer_profile.py" --compiler $clang --output-dir "$root/local"
        if ($LASTEXITCODE -ne 0) { throw 'Main producer phase recorder regression failed.' }
        & $python "$root/tests/test_vp_packet_oracle.py"
        if ($LASTEXITCODE -ne 0) { throw 'Vertex-program packet oracle guard tests failed.' }
        & $python "$root/scripts/patch.py" oracle-vp-packet "$assets/generated/ppu/ppu_recomp_003.cpp" "$root/local/vp_packet_reference.inc"
        if ($LASTEXITCODE -ne 0) { throw 'Vertex-program packet oracle extraction failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++17 "/I$root/port" "/I$root/local" "/Fe$root/local/test_vp_packets.exe" "/Fo$root/local/test_vp_packets.obj" "$root/tests/test_vp_packets.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Vertex-program packet test compilation failed.' }
        & "$root/local/test_vp_packets.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Vertex-program packet boundary comparison failed.' }
        & $python "$root/tests/test_vp_upload_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Vertex-program upload patch guards failed.' }
        & $clang /nologo /EHsc /W0 /O2 /std:c++17 "/I$assets/generated/ppu" "/I$sdk/runtime/ppu" "/I$root/local" "/Fe$root/local/test_vp_upload.exe" "/Fo$root/local/" "$root/tests/test_vp_upload.cpp" "$root/port/haze_vp_upload.cpp" "$sdk/runtime/ppu/ppu_native_memory.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Native vertex-program upload adapter compilation failed.' }
        & "$root/local/test_vp_upload.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native vertex-program upload boundary validation failed.' }
    }
    $audioOutTest = Join-Path $root 'local/test_audio_out_abi.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$audioOutTest" "/Fo$root/local/test_audio_out_abi.obj" "$sdk/libs/misc/tests/test_audio_out_abi.c"
    if ($LASTEXITCODE -ne 0) { throw 'Audio-output ABI test compilation failed.' }
    & $audioOutTest
    if ($LASTEXITCODE -ne 0) { throw 'Audio-output device/state ABI test failed.' }
    $padTest = Join-Path $root 'local/test_host_input.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$padTest" "/Fo$root/local/test_host_input.obj" "$sdk/libs/input/tests/test_host_input.c" /link user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Host pad input test compilation failed.' }
    & $padTest
    if ($LASTEXITCODE -ne 0) { throw 'Host pad input packet test failed.' }
    $hazeInputTest = Join-Path $root 'local/test_haze_input.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/I$sdk/include" "/I$sdk/libs/input" "/I$sdk/libs/codec" "/Fe$hazeInputTest" "/Fo$root/local/test_haze_input.obj" "$root/tests/test_haze_input.cpp" /link user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Haze keyboard/mouse test compilation failed.' }
    & $hazeInputTest
    if ($LASTEXITCODE -ne 0) { throw 'Haze keyboard/mouse mapping test failed.' }
    $hazeCameraTest = Join-Path $root 'local/test_haze_camera.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/I$root/tests/fixtures/camera" "/I$sdk/include" "/I$sdk/libs/input" "/I$sdk/libs/codec" "/Fe$hazeCameraTest" "/Fo$root/local/test_haze_camera.obj" "$root/tests/test_haze_camera.cpp" /link user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Native camera test compilation failed.' }
    & $hazeCameraTest
    if ($LASTEXITCODE -ne 0) { throw 'Native camera gating/rotation test failed.' }
    $hazeMenuTest = Join-Path $root 'local/test_haze_menu.exe'
    & $clang /nologo /W0 /O1 /std:c++20 "/I$root/tests/fixtures/camera" "/I$sdk/include" "/I$sdk/libs/input" "/I$sdk/libs/codec" "/I$sdk/libs/video" "/Fe$hazeMenuTest" "/Fo$root/local/test_haze_menu.obj" "$root/tests/test_haze_menu.cpp" /link user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Native menu test compilation failed.' }
    & $hazeMenuTest
    if ($LASTEXITCODE -ne 0) { throw 'Native menu input regression failed.' }
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_menu_input_patch.py" "$assets/generated/ppu/ppu_recomp_001.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Native menu patch guard regression failed.' }
    }
    & $python "$sdk/tools/test_spu_text_selection.py"
    if ($LASTEXITCODE -ne 0) { throw 'SPU segment-selection regression tests failed.' }
    & $python "$sdk/tools/test_spu_trace_registers.py"
    if ($LASTEXITCODE -ne 0) { throw 'SPU destination-register trace tests failed.' }
    $oldCC = $env:CC
    try {
        $env:CC = Join-Path (Split-Path $clang -Parent) 'clang.exe'
        foreach ($vectorTest in @('test_ppu_vsum4ubs.py', 'test_ppu_vsl.py')) {
            & $python "$sdk/tools/$vectorTest"
            if ($LASTEXITCODE -ne 0) { throw "PPU vector regression failed: $vectorTest" }
        }
    } finally { $env:CC = $oldCC }
    $condTest = Join-Path $root 'local/test_sysprx_lwcond.exe'
    # Generate only the SDK ABI declarations, without reading a game binary.
    $abi = Join-Path $root 'build/test-abi'
    & $python "$root/scripts/test-abi.py" $sdk $abi
    if ($LASTEXITCODE -ne 0) { throw 'Test ABI header generation failed.' }
    & $clang /nologo /W0 /O1 /Gy /Gw "/I$abi" "/I$sdk/include" "/Fe$condTest" "/Fo$root/local/test_sysprx_lwcond.obj" "$sdk/runtime/ppu/tests/test_sysprx_lwcond.cpp" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'Lightweight condition-variable test compilation failed.' }
    & $condTest
    if ($LASTEXITCODE -ne 0) { throw 'Lightweight condition-variable boundary and wait test failed.' }
    $queueTest = Join-Path $root 'local/test_queue_size.exe'
    New-Item -ItemType Directory -Path (Split-Path $queueTest -Parent) -Force | Out-Null
    & $clang /nologo /W0 "/I$sdk/include" "/I$sdk/runtime/spu" "/Fe$queueTest" "/Fo$root/local/test_queue_size.obj" "$sdk/libs/spurs/tests/test_queue_size.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPURS queue query test compilation failed.' }
    & $queueTest
    if ($LASTEXITCODE -ne 0) { throw 'SPURS queue query contract test failed.' }
    $queueProfileTest = Join-Path $root 'local/test_queue_profile.exe'
    & $clang /nologo /W0 /O2 "/I$sdk/include" "/I$sdk/runtime/spu" "/Fe$queueProfileTest" "/Fo$root/local/test_queue_profile.obj" "$sdk/libs/spurs/tests/test_queue_profile.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPURS queue timing test compilation failed.' }
    $oldQueueProfile = $env:SPURS_QUEUE_PROFILE
    $oldQueueControl = $env:SPURS_QUEUE_PROFILE_CONTROL
    try {
        $env:SPURS_QUEUE_PROFILE = Join-Path $root 'local/queue-profile-test'
        $env:SPURS_QUEUE_PROFILE_CONTROL = $null
        New-Item -ItemType Directory -Force $env:SPURS_QUEUE_PROFILE | Out-Null
        & $queueProfileTest
        if ($LASTEXITCODE -ne 0) { throw 'SPURS queue timing attribution test failed.' }
    } finally {
        $env:SPURS_QUEUE_PROFILE = $oldQueueProfile
        $env:SPURS_QUEUE_PROFILE_CONTROL = $oldQueueControl
    }
    $nativeQueueTest = Join-Path $root 'local/test_native_queue_wait.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$nativeQueueTest" "/Fo$root/local/" "$sdk/libs/spurs/tests/test_native_queue_wait.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'Native queue wake test compilation failed.' }
    & $nativeQueueTest
    if ($LASTEXITCODE -ne 0) { throw 'Native queue publication/wake/lifetime test failed.' }
    $gameDataTest = Join-Path $root 'local/test_gamedata_callback.exe'
    & $clang /nologo /W0 "/I$sdk/include" "/Fe$gameDataTest" "/Fo$root/local/test_gamedata_callback.obj" "$sdk/libs/system/tests/test_gamedata_callback.c"
    if ($LASTEXITCODE -ne 0) { throw 'Game-data callback test compilation failed.' }
    & $gameDataTest
    if ($LASTEXITCODE -ne 0) { throw 'Game-data callback ABI test failed.' }
    $dmaTest = Join-Path $root 'local/test_dma_repeat.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/I$sdk/runtime/platform" "/Fe$dmaTest" "/Fo$root/local/test_dma_repeat.obj" "$sdk/runtime/spu/tests/test_dma_repeat.c"
    if ($LASTEXITCODE -ne 0) { throw 'DMA repeat-watchdog test compilation failed.' }
    & $dmaTest
    if ($LASTEXITCODE -ne 0) { throw 'DMA repeat-watchdog contract test failed.' }
    $mfcTest = Join-Path $root 'local/test_mfc_context_state.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$mfcTest" "/Fo$root/local/test_mfc_context_state.obj" "$sdk/runtime/spu/tests/test_mfc_context_state.c"
    if ($LASTEXITCODE -ne 0) { throw 'Private MFC state test compilation failed.' }
    & $mfcTest
    if ($LASTEXITCODE -ne 0) { throw 'Private MFC context isolation test failed.' }
    $dispatchTest = Join-Path $root 'local/test_taskset_dispatch.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/I$sdk/runtime/platform" "/Fe$dispatchTest" "/Fo$root/local/test_taskset_dispatch.obj" "$sdk/runtime/spu/tests/test_taskset_dispatch.c"
    if ($LASTEXITCODE -ne 0) { throw 'Taskset dispatch test compilation failed.' }
    & $dispatchTest
    if ($LASTEXITCODE -ne 0) { throw 'Taskset dispatch contract test failed.' }
    $publicationTest = Join-Path $root 'local/test_taskset_publication.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$publicationTest" "/Fo$root/local/test_taskset_publication.obj" "$sdk/runtime/spu/tests/test_taskset_publication.c"
    if ($LASTEXITCODE -ne 0) { throw 'Concurrent task publication test compilation failed.' }
    $oldVerbose = $env:PS3_VERBOSE
    try {
        $env:PS3_VERBOSE = '0'
        & $publicationTest
        if ($LASTEXITCODE -ne 0) { throw 'Concurrent task publication and recycling test failed.' }
    } finally { $env:PS3_VERBOSE = $oldVerbose }
    & $python "$sdk/runtime/spu/tests/test_taskset_wake_watch.py" --compiler $clang
    if ($LASTEXITCODE -ne 0) { throw 'Taskset readiness watcher test failed.' }
    & $python "$sdk/runtime/spu/tests/test_taskset_wake_integration.py" --compiler $clang
    if ($LASTEXITCODE -ne 0) { throw 'Taskset wake policy/dispatch integration test failed.' }
    $drawBatchTest = Join-Path $root 'local/test_draw_batches.exe'
    & $clang /nologo /W0 /O1 "/Fe$drawBatchTest" "/Fo$root/local/test_draw_batches.obj" "$sdk/libs/video/tests/test_draw_batches.c"
    if ($LASTEXITCODE -ne 0) { throw 'Draw-packet storage test compilation failed.' }
    & $drawBatchTest
    if ($LASTEXITCODE -ne 0) { throw 'Draw-packet growth and order test failed.' }
    $m2mfTest = Join-Path $root 'local/test_m2mf.exe'
    $scaledBlitTest = Join-Path $root 'local/test_scaled_blit.exe'
    & $clang /nologo /W0 /O2 /Gy /Gw "/I$sdk/include" "/Fe$scaledBlitTest" "/Fo$root/local/test_scaled_blit.obj" "$sdk/libs/video/tests/test_scaled_blit.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'Scaled blit test compilation failed.' }
    & $scaledBlitTest
    if ($LASTEXITCODE -ne 0) { throw 'Scaled blit semantics/coherency test failed.' }
    & $clang /nologo /W0 /O1 "/Fe$m2mfTest" "/Fo$root/local/test_m2mf.obj" "$sdk/libs/video/tests/test_m2mf.c"
    if ($LASTEXITCODE -ne 0) { throw 'GPU buffer-copy test compilation failed.' }
    & $m2mfTest
    if ($LASTEXITCODE -ne 0) { throw 'GPU buffer-copy contract test failed.' }
    $fifoTest = Join-Path $root 'local/test_fifo_recycle.exe'
    & $clang /nologo /W0 /O1 /Gy /Gw "/I$sdk/include" "/Fe$fifoTest" "/Fo$root/local/test_fifo_recycle.obj" "$sdk/libs/video/tests/test_fifo_recycle.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'FIFO recycling test compilation failed.' }
    foreach ($nativeFifoWait in @(0,1)) {
        & $fifoTest $nativeFifoWait
        if ($LASTEXITCODE -ne 0) { throw 'FIFO recycling consumer acknowledgment/native wait test failed.' }
    }
    $fpTest = Join-Path $root 'local/test_fp_decompiler.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$fpTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_decompiler.c" "$sdk/libs/video/rsx_fp_decompiler.c"
    if ($LASTEXITCODE -ne 0) { throw 'Fragment decompiler test compilation failed.' }
    & $fpTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment decompiler regression failed.' }
    foreach ($refsScalar in @(0,1)) {
        $refsTest = Join-Path $root "local/test_vertex_refs_$refsScalar.exe"
        $refsFlags = @()
        if ($refsScalar) { $refsFlags += '/DRSX_REFS_SCALAR=1' }
        & $clang /nologo /W0 /O2 /std:c17 @refsFlags "/I$sdk/include" "/Fe$refsTest" "/Fo$root/local/test_vertex_refs_$refsScalar.obj" "$sdk/libs/video/tests/test_vertex_refs.c"
        if ($LASTEXITCODE -ne 0) { throw 'Native index-reference test compilation failed.' }
        & $refsTest
        if ($LASTEXITCODE -ne 0) { throw 'Native index-reference oracle failed.' }
    }
    $vertexPullTest = Join-Path $root 'local/test_vertex_pull.exe'
    & $clang /nologo /EHsc /W0 /O2 /Gy /clang:-msse4.1 "/I$sdk/include" "/Fe$vertexPullTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_vertex_pull.cpp" "$sdk/libs/video/rsx_vertex_compact.c" "$sdk/libs/video/rsx_dispatch.c" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'GPU vertex decoder oracle compilation failed.' }
    & $vertexPullTest
    if ($LASTEXITCODE -ne 0) { throw 'GPU vertex decoder WARP oracle failed.' }
    $predicateTest = Join-Path $root 'local/test_fp_predication.exe'
    & $clang /nologo /EHsc /W0 /O1 "/I$sdk/include" "/Fe$predicateTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_predication.cpp" "$sdk/libs/video/rsx_fp_decompiler.c" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'Fragment predication test compilation failed.' }
    & $predicateTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment predication shader execution failed.' }
    $shadowTest = Join-Path $root 'local/test_fp_shadow.exe'
    & $clang /nologo /EHsc /W0 /O1 "/I$sdk/include" "/Fe$shadowTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_shadow.cpp" "$sdk/libs/video/rsx_fp_decompiler.c" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'Fragment shadow test compilation failed.' }
    & $shadowTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment shadow comparison GPU test failed.' }
    $depthExportTest = Join-Path $root 'local/test_fp_depth.exe'
    & $clang /nologo /EHsc /W0 /O1 "/I$sdk/include" "/Fe$depthExportTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_depth.cpp" "$sdk/libs/video/rsx_fp_decompiler.c" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'Fragment depth-export test compilation failed.' }
    & $depthExportTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment depth-export GPU test failed.' }
    $windowTest = Join-Path $root 'local/test_fp_window.exe'
    & $clang /nologo /EHsc /W0 /O1 "/I$sdk/include" "/Fe$windowTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_window.cpp" "$sdk/libs/video/rsx_fp_decompiler.c" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'Fragment window-position test compilation failed.' }
    & $windowTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment window-position GPU test failed.' }
    $clearTest = Join-Path $root 'local/test_color_clear.exe'
    & $clang /nologo /W0 /O1 "/Fe$clearTest" "/Fo$root/local/test_color_clear.obj" "$sdk/libs/video/tests/test_color_clear.c" /link d3d12.lib dxgi.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw 'Color clear test compilation failed.' }
    & $clearTest
    if ($LASTEXITCODE -ne 0) { throw 'D3D12 channel/scissor clear execution failed.' }
    $textureTest = Join-Path $root 'local/test_live_textures.exe'
    $frameLimitTest = Join-Path $root 'local/test_frame_limiter.exe'
    & $clang /nologo /W0 /O2 "/Fe$frameLimitTest" "/Fo$root/local/test_frame_limiter.obj" "$sdk/libs/video/tests/test_frame_limiter.c"
    if ($LASTEXITCODE -ne 0) { throw 'Frame limiter test compilation failed.' }
    & $frameLimitTest
    if ($LASTEXITCODE -ne 0) { throw 'Frame limiter deadline/wait test failed.' }
    $vpCacheTest = Join-Path $root 'local/test_vp_program_cache.exe'
    & $clang /nologo /W0 /O2 "/I$sdk/include" "/Fe$vpCacheTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_vp_program_cache.c" "$sdk/libs/video/rsx_vp_program_cache.c" "$sdk/libs/video/rsx_vp_decompiler.c"
    if ($LASTEXITCODE -ne 0) { throw 'Vertex program cache test compilation failed.' }
    & $vpCacheTest
    if ($LASTEXITCODE -ne 0) { throw 'Vertex program cache content validation failed.' }
    $textureLibrary = Join-Path $build $(if ($RuntimeOnly) { 'ps3recomp_runtime.lib' } else { 'ps3recomp_sdk/ps3recomp_runtime.lib' })
    & $clang /nologo /W0 /O1 /MD /Gy /Gw "/I$sdk/include" "/Fe$textureTest" "/Fo$root/local/test_live_textures.obj" "$sdk/libs/video/tests/test_live_textures.c" /link /OPT:REF $textureLibrary
    if ($LASTEXITCODE -ne 0) { throw 'Live texture test compilation failed.' }
    & $textureTest
    if ($LASTEXITCODE -ne 0) { throw 'Live texture cache and D3D12 upload test failed.' }
    $watchTest = Join-Path $root 'local/test_native_write_watch.exe'
    & $clang /nologo /W3 /O2 /MD /EHsc /std:c++17 "/I$sdk/libs/video" "/Fe$watchTest" "/Fo$root/local/test_native_write_watch.obj" "$root/tests/benchmark_write_watch.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Native write-watch test compilation failed.' }
    & $watchTest --test-only
    if ($LASTEXITCODE -ne 0) { throw 'Native write-watch mutation oracle failed.' }
    $saveTest = Join-Path $root 'local/test_savedata_persistence.exe'
    & $clang /nologo /W0 /O1 /Gy /Gw "/I$sdk/include" "/Fe$saveTest" "/Fo$root/local/test_savedata_persistence.obj" "$sdk/libs/system/tests/test_savedata_persistence.c" /link /OPT:REF
    if ($LASTEXITCODE -ne 0) { throw 'Save persistence test compilation failed.' }
    & $saveTest
    if ($LASTEXITCODE -ne 0) { throw 'Save persistence and missing-load result test failed.' }
    $sailTest = Join-Path $root 'local/test_sail_lifecycle.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$sailTest" "/Fo$root/local/test_sail_lifecycle.obj" "$sdk/libs/codec/tests/test_sail_lifecycle.c"
    if ($LASTEXITCODE -ne 0) { throw 'SAIL lifecycle test compilation failed.' }
    & $sailTest
    if ($LASTEXITCODE -ne 0) { throw 'SAIL lifecycle ABI test failed.' }
    $movieSailTest = Join-Path $root 'local/test_movie_sail.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/I$sdk/libs/codec" "/Fe$movieSailTest" "/Fo$root/local/test_movie_sail.obj" "$root/tests/test_movie_sail.c"
    if ($LASTEXITCODE -ne 0) { throw 'Haze movie lifecycle test compilation failed.' }
    & $movieSailTest
    if ($LASTEXITCODE -ne 0) { throw 'Haze movie lifecycle regression failed.' }
    $moviePamfTest = Join-Path $root 'local/test_movie_pamf.exe'
    & $clang /nologo /W0 /O1 /EHsc /std:c++20 "/I$root/port" "/Fe$moviePamfTest" "/Fo$root/local/test_movie_pamf.obj" "$root/tests/test_movie_pamf.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'PAMF parser test compilation failed.' }
    & $moviePamfTest
    if ($LASTEXITCODE -ne 0) { throw 'PAMF parser regression failed.' }
    $dialogTest = Join-Path $root 'local/test_dialog_callbacks.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$dialogTest" "/Fo$root/local/test_dialog_callbacks.obj" "$sdk/libs/system/tests/test_dialog_callbacks.c"
    if ($LASTEXITCODE -ne 0) { throw 'Dialog callback test compilation failed.' }
    & $dialogTest
    if ($LASTEXITCODE -ne 0) { throw 'Dialog callback order test failed.' }
    $fileTest = Join-Path $root 'local/test_host_file64.exe'
    & $clang /nologo /W0 /O1 "/Fe$fileTest" "/Fo$root/local/test_host_file64.obj" "$sdk/runtime/platform/tests/test_host_file64.c"
    if ($LASTEXITCODE -ne 0) { throw 'Large-file test compilation failed.' }
    & $fileTest
    if ($LASTEXITCODE -ne 0) { throw 'Large-file stat and seek test failed.' }
    $memoryTest = Join-Path $root 'local/test_memory_container.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$memoryTest" "/Fo$root/local/test_memory_container.obj" "$sdk/runtime/syscalls/tests/test_memory_container.c"
    if ($LASTEXITCODE -ne 0) { throw 'Memory-container test compilation failed.' }
    & $memoryTest
    if ($LASTEXITCODE -ne 0) { throw 'Memory-container allocation test failed.' }
    $ovisTest = Join-Path $root 'local/test_ovis_elf.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$ovisTest" "/Fo$root/local/test_ovis_elf.obj" "$sdk/libs/misc/tests/test_ovis_elf.c"
    if ($LASTEXITCODE -ne 0) { throw 'Overlay ELF query test compilation failed.' }
    & $ovisTest
    if ($LASTEXITCODE -ne 0) { throw 'Overlay ELF query ABI test failed.' }
    $elfTest = Join-Path $root 'local/test_elf_overlay.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$elfTest" "/Fo$root/local/test_elf_overlay.obj" "$sdk/runtime/spu/tests/test_elf_overlay.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU ELF loader test compilation failed.' }
    & $elfTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU ELF overlay isolation test failed.' }
    $bindingTest = Join-Path $root 'local/test_overlay_binding.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$bindingTest" "/Fo$root/local/test_overlay_binding.obj" "$sdk/runtime/spu/tests/test_overlay_binding.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU overlay binding test compilation failed.' }
    & $bindingTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU overlay content binding test failed.' }
    $lookupTest = Join-Path $root 'local/test_code_lookup.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$lookupTest" "/Fo$root/local/test_code_lookup.obj" "$sdk/runtime/spu/tests/test_code_lookup.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU resident lookup test compilation failed.' }
    & $lookupTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU resident lookup test failed.' }
    Write-Output 'SPU segment selection, SPURS queues, game-data callbacks, DMA watchdog and isolation, taskset dispatch, SAIL lifecycle, dialog callbacks, large-file, memory-container, overlay staging/binding, and SPU ELF loader tests passed.'
}

