param(
    [string]$SharedRoot,
    [string]$SdkRoot,
    [string]$SDL3Root,
    [string]$FFmpegRoot,
    [string]$Python,
    [switch]$RuntimeOnly,
    [switch]$Test,
    [switch]$ProfileRenderer,
    [switch]$CompareSpuByteOps,
    [switch]$SpuFma,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [string]$OutputDirectory,
    [int]$Jobs = 8
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
if ($SDL3Root) { $controllerArgs += "-DFETCHCONTENT_SOURCE_DIR_SDL3=$((Resolve-Path -LiteralPath $SDL3Root).Path)" }
if ($FFmpegRoot) { $controllerArgs += "-DHAZE_FFMPEG_ROOT=$((Resolve-Path -LiteralPath $FFmpegRoot).Path)" }
& $cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$exeOutput" "-DPython3_EXECUTABLE=$python" "-DPS3RECOMP_DIR=$sdk" "-DRECOMP_DIR=$assets/generated/ppu" "-DSPU_DIR=$assets/generated/spu" "-DHAZE_SHARED_ROOT=$assets" "-DHAZE_RSX_PROFILE=$([int][bool]$ProfileRenderer)" "-DHAZE_SPU_FMA=$([int][bool]$SpuFma)" "-DHAZE_SPU_BYTE_COMPARE=$([int][bool]$CompareSpuByteOps)" @controllerArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
if (-not $RuntimeOnly) {
    & $python "$root/tests/check_profile_configuration.py" $build --enabled $([int][bool]$ProfileRenderer)
    if ($LASTEXITCODE -ne 0) { throw 'Profiling definitions do not match the requested build.' }
}
& $cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
if ($Test) {
    New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_movie_patch.py" "$assets/generated/ppu/ppu_recomp_003.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Movie guest EOF patch regression failed.' }
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
    $audioRingTest = Join-Path $root 'local/test_audio_ring.exe'
    & $clang /nologo /W0 /O2 /std:c11 /Gy /Gw "/I$sdk/include" "/Fe$audioRingTest" "/Fo$root/local/test_audio_ring.obj" "$sdk/libs/audio/tests/test_audio_ring.c" /link /OPT:REF ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Audio ring/routing test compilation failed.' }
    & $audioRingTest
    if ($LASTEXITCODE -ne 0) { throw 'Audio ring/routing regression failed.' }
    $nativeFloatTest = Join-Path $root 'local/test_spu_float_native.exe'
    & $clang /nologo /W0 /O2 /Ob1 /DSPU_FLOAT_NATIVE_RUNTIME=1 /clang:-msse4.1 "/I$sdk/include" "/Fe$nativeFloatTest" "/Fo$root/local/test_spu_float_native.obj" "$sdk/runtime/spu/tests/test_spu_float_native.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU native arithmetic test compilation failed.' }
    & $nativeFloatTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU native arithmetic/reference regression failed.' }
    & $python "$root/tests/test_wait_timeline.py"
    if ($LASTEXITCODE -ne 0) { throw 'Wait timeline syscall naming and interval-union tests failed.' }
    $graphicsTest = Join-Path $root 'local/test_haze_graphics.exe'
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
    $coherencyTest = Join-Path $root 'local/test_coherency_lifetime.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/include" "/Fe$coherencyTest" "/Fo$root/local/" "$sdk/runtime/spu/tests/test_coherency_lifetime.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency lifetime test compilation failed.' }
    & $coherencyTest
    if ($LASTEXITCODE -ne 0) { throw 'SPU coherency lifetime/holder-capacity test failed.' }
    $idleTest = Join-Path $root 'local/test_spu_idle_wait.exe'
    & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/runtime/spu" "/Fe$idleTest" "/Fo$root/local/" "$root/tests/test_spu_idle_wait.c" "$sdk/runtime/spu/spu_coherency.c"
    if ($LASTEXITCODE -ne 0) { throw 'Haze SPU idle wait test compilation failed.' }
    & $idleTest
    if ($LASTEXITCODE -ne 0) { throw 'Haze SPU idle wait predicate/publication test failed.' }
    if (-not $RuntimeOnly) {
        $tripletOracle = Join-Path $root 'local/triplet_oracle.c'
        & $python "$root/scripts/prepare_triplet_oracle.py" "$assets/generated/spu/spu_0041_at_00F2E380/spu_recomp.c" $tripletOracle
        if ($LASTEXITCODE -ne 0) { throw 'Audited triplet-copy oracle extraction failed.' }
        $tripletTest = Join-Path $root 'local/test_spu_triplet_copy.exe'
        & $clang /nologo /W0 /O2 /clang:-msse4.1 "/I$sdk/runtime/spu" "/I$root/port" "/Fe$tripletTest" "/Fo$root/local/" "$root/tests/test_spu_triplet_copy.c" $tripletOracle
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy oracle compilation failed.' }
        & $tripletTest
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy full-state oracle failed.' }
        & $python "$root/tests/test_spu_triplet_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Native triplet-copy patch guards failed.' }
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
    & $clang /nologo /W0 /O1 /std:c++20 "/Fe$frameTest" "/Fo$root/local/test_frame_pump.obj" "$root/tests/test_frame_pump.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'Frame-pump test compilation failed.' }
    & $frameTest
    if ($LASTEXITCODE -ne 0) { throw 'Frame-boundary presentation test failed.' }
    & $python "$root/tests/test_firmware_relocation.py"
    if ($LASTEXITCODE -ne 0) { throw 'Firmware ELF relocation tests failed.' }
    & $python "$root/tests/test_present_modes.py"
    if ($LASTEXITCODE -ne 0) { throw 'Present-window benchmark validation failed.' }
    if (-not $RuntimeOnly) {
        & $python "$root/tests/test_guest_pacer_patch.py"
        if ($LASTEXITCODE -ne 0) { throw 'Guest pacing patch guard validation failed.' }
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
    & $fifoTest
    if ($LASTEXITCODE -ne 0) { throw 'FIFO recycling consumer acknowledgment test failed.' }
    $fpTest = Join-Path $root 'local/test_fp_decompiler.exe'
    & $clang /nologo /W0 /O1 "/I$sdk/include" "/Fe$fpTest" "/Fo$root/local/" "$sdk/libs/video/tests/test_fp_decompiler.c" "$sdk/libs/video/rsx_fp_decompiler.c"
    if ($LASTEXITCODE -ne 0) { throw 'Fragment decompiler test compilation failed.' }
    & $fpTest
    if ($LASTEXITCODE -ne 0) { throw 'Fragment decompiler regression failed.' }
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
    & $clang /nologo /W0 /O1 /std:c++20 "/I$root/port" "/Fe$moviePamfTest" "/Fo$root/local/test_movie_pamf.obj" "$root/tests/test_movie_pamf.cpp"
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

