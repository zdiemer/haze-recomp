param([string]$SharedRoot, [string]$SDL3Root, [string]$SdkRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$assets=if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
$sdk=if ($SdkRoot) { (Resolve-Path -LiteralPath $SdkRoot).Path } elseif (Test-Path -LiteralPath "$root/external/ps3recomp/CMakeLists.txt") { Join-Path $root 'external/ps3recomp' } else { Join-Path $assets 'external/ps3recomp' }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}

$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$controllerArgs=@()
if ($SDL3Root) { $controllerArgs += "-DFETCHCONTENT_SOURCE_DIR_SDL3=$((Resolve-Path -LiteralPath $SDL3Root).Path)" }
& $cmake -S "$root/tests/controllers" -B "$root/build/input-tests" "-DPS3RECOMP_DIR=$sdk" @controllerArgs
if ($LASTEXITCODE -ne 0) { throw 'Controller test configuration failed.' }
& $cmake --build "$root/build/input-tests" --config Debug --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Controller test compilation failed.' }
$ctest=Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
& $ctest --test-dir "$root/build/input-tests" -C Debug --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Controller tests failed.' }
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
$ErrorActionPreference='Continue' # Windows PowerShell promotes native stderr warnings.
foreach ($test in @('test_haze_input','test_haze_camera','test_haze_graphics','test_haze_video_backend','test_haze_display_preview','test_haze_menu_guest','test_haze_menu','test_haze_save_status','test_haze_controls','test_haze_quit','test_haze_audio_shutdown','test_haze_quit_ui')) {
    & $clang /nologo /W4 /O1 /std:c++20 "/I$root/tests/fixtures/camera" "/I$root/tests/fixtures/video" "/I$root/tests/fixtures/audio" "/I$sdk/include" "/I$sdk/libs/input" "/I$sdk/libs/codec" "/I$sdk/libs/video" "/Fe$root/local/$test.exe" "/Fo$root/local/$test.obj" "$root/tests/$test.cpp" /link user32.lib gdi32.lib comdlg32.lib
    if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
    & "$root/local/$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}
& "$assets/.venv/Scripts/python.exe" "$root/tests/test_menu_page_patch.py" "$assets/generated/ppu/ppu_recomp_001.cpp"
if ($LASTEXITCODE -ne 0) { throw 'Menu page patch guard failed.' }
& "$assets/.venv/Scripts/python.exe" "$root/tests/test_save_status_patch.py" "$assets/generated/ppu/ppu_recomp_003.cpp"
if ($LASTEXITCODE -ne 0) { throw 'Save status patch guard failed.' }
& "$assets/.venv/Scripts/python.exe" "$root/tests/test_audio_tick_patch.py" "$assets/generated/ppu/ppu_recomp_003.cpp"
if ($LASTEXITCODE -ne 0) { throw 'Audio tick patch guard failed.' }
foreach ($test in @('test_haze_audio','test_haze_quit_guest')) {
    & $clang /nologo /W4 /O1 /std:c++20 "/I$assets/generated/ppu" "/I$sdk/include" "/I$sdk/libs/input" "/Fe$root/local/$test.exe" "/Fo$root/local/$test.obj" "$root/tests/$test.cpp"
    if ($LASTEXITCODE -ne 0) { throw "$test compilation failed." }
    & "$root/local/$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$test failed." }
}
exit 0 # Native compiler warnings on stderr are not a failed test run.
