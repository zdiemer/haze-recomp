param([string]$SharedRoot,[switch]$Test,[ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$assets=if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
if ($env:HAZE_LLVM_ROOT) { $clang = Join-Path $env:HAZE_LLVM_ROOT 'bin/clang-cl.exe' }
& $cmake -S "$root/port/launcher" -B "$root/build/launcher" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$clang" "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$root/build/launcher/bin" "-DHAZE_SHARED_ROOT=$assets" "-DHAZE_LAUNCHER_TESTS=$([int][bool]$Test)"
if ($LASTEXITCODE -ne 0) { throw 'Launcher configuration failed.' }
& $cmake --build "$root/build/launcher" --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
if ($Test) {
    $ctest=Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
    & $ctest --test-dir "$root/build/launcher" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Launcher tests failed.' }
}
exit 0
