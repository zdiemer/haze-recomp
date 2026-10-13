param([string]$SharedRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$assets=if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$cmake=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja=Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
if ($env:HAZE_LLVM_ROOT) { $clang = Join-Path $env:HAZE_LLVM_ROOT 'bin/clang-cl.exe' }
& $cmake -S "$root/tests/icon" -B "$root/build/icon-tests" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$clang" "-DHAZE_SHARED_ROOT=$assets"
if ($LASTEXITCODE -ne 0) { throw 'Icon fixture configuration failed.' }
& $cmake --build "$root/build/icon-tests"
if ($LASTEXITCODE -ne 0) { throw 'Icon fixture build failed.' }
& "$root/build/icon-tests/HazeIconTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Windows icon resource checks failed.' }
& "$root/tests/test_icon_conversion.ps1" -Converter "$root/scripts/build-icon.ps1" -Directory "$root/build/icon-tests/conversion"
if (-not $?) { throw 'Icon conversion checks failed.' }
# Exercise dependency tracking in a separate clean build with local fixture art.
$fixture="$root/build/icon-tests/fixture assets"
$fixtureBuild="$root/build/icon-tests/fallback-build"
$fixturePng="$fixture/game/disc/PS3_GAME/ICON0.PNG"
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fixturePng)) | Out-Null
if ([IO.File]::Exists($fixturePng)) { [IO.File]::Delete($fixturePng) }
& $cmake -S "$root/tests/icon" -B $fixtureBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$clang" "-DHAZE_SHARED_ROOT=$fixture"
if ($LASTEXITCODE -ne 0) { throw 'Fallback fixture configuration failed.' }
$probe="$fixtureBuild/HazeIconTests.exe"
$ico="$fixtureBuild/HazeIconTests-icon/app.ico"
for ($step=0;$step -lt 3;$step++) {
    if ($step -eq 1) {
        $art=[Drawing.Bitmap]::new(80,40); $dc=[Drawing.Graphics]::FromImage($art)
        try { $dc.Clear([Drawing.Color]::Blue); $art.Save($fixturePng,[Drawing.Imaging.ImageFormat]::Png) }
        finally { $dc.Dispose(); $art.Dispose() }
    } elseif ($step -eq 2) { [IO.File]::Delete($fixturePng) }
    & $cmake --build $fixtureBuild
    if ($LASTEXITCODE -ne 0) { throw 'Fallback/add/remove resource build failed.' }
    & $probe --icon-file $ico $probe
    if ($LASTEXITCODE -ne 0) { throw 'Executable retained stale icon layers.' }
    $hash=(Get-FileHash -LiteralPath $ico).Hash
    if ($step -eq 0) { $fallbackHash=$hash }
    elseif (($step -eq 1 -and $hash -eq $fallbackHash) -or ($step -eq 2 -and $hash -ne $fallbackHash)) { throw 'Artwork discovery/removal failed.' }
}
exit 0
