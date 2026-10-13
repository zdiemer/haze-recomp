param([string]$SdkRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$sdk=if ($SdkRoot) { (Resolve-Path -LiteralPath $SdkRoot).Path } else { Join-Path $root 'external/ps3recomp' }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
if ($env:HAZE_LLVM_ROOT) { $clang = Join-Path $env:HAZE_LLVM_ROOT 'bin/clang-cl.exe' }
New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
$ErrorActionPreference='Continue'
Push-Location $root
try {
    foreach ($test in @('test_haze_steam','test_haze_steam_art','test_haze_steam_menu')) {
        & $clang /nologo /W4 /O1 /EHsc /std:c++20 "/I$sdk/include" "/I$sdk/libs/input" "/Fe$root/local/$test.exe" "/Fo$root/local/$test.obj" "$root/tests/$test.cpp" /link advapi32.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
        if ($LASTEXITCODE -ne 0) { throw "$test compilation failed." }
        & "$root/local/$test.exe"
        if ($LASTEXITCODE -ne 0) { throw "$test failed." }
    }
} finally { Pop-Location }
exit 0
