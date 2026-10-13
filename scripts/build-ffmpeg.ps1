param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$MsysRoot = 'C:/msys64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize MSVC.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$llvm = if ($env:HAZE_LLVM_ROOT) { $env:HAZE_LLVM_ROOT } else { Join-Path $vs 'VC/Tools/Llvm/x64' }
$env:PATH = "$llvm/bin;$MsysRoot/usr/bin;$env:PATH"
$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new FFmpeg output directory.' }
& "$MsysRoot/usr/bin/bash.exe" "$root/scripts/build-ffmpeg.sh" $sourcePath "$output-build" $output
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg compilation failed.' }
# FFmpeg's MSVC install puts import libraries alongside the DLLs. The game
# dependency contract expects those libraries under lib/.
Copy-Item -Path "$output/bin/*.lib" -Destination "$output/lib"
foreach ($name in @('COPYING.LGPLv2.1', 'COPYING.LGPLv3', 'LICENSE.md')) {
    Copy-Item -LiteralPath "$sourcePath/$name" -Destination $output
}
Copy-Item -LiteralPath "$output-build/ffbuild/config.log" -Destination "$output/build-config.log"
exit 0
