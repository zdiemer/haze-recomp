param(
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$Launcher,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new runtime staging directory.' }
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$launcherPath = (Resolve-Path -LiteralPath $Launcher).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'The release builder requires Visual Studio C++ tools.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the compiler environment.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$redist = Get-ChildItem -Path "$env:VCToolsRedistDir/x64/Microsoft.VC*.CRT" -Directory | Select-Object -First 1
if (-not $redist) { throw 'Cannot locate the app-local x64 VC runtime.' }
New-Item -ItemType Directory $output | Out-Null
# Place CRT next to the staged executables before scanning: Windows/System32
# may already contain another version, which must not mask these dependencies.
Copy-Item -LiteralPath $gamePath,$launcherPath -Destination $output
Copy-Item -Path "$($redist.FullName)/*.dll" -Destination $output
$search = @((Split-Path $gamePath), (Split-Path $launcherPath), $redist.FullName) -join ';'
& $cmake "-DGAME=$output/HazeRecomp.exe" "-DLAUNCHER=$output/HazeLauncher.exe" "-DSEARCH_DIRECTORIES=$search" "-DOUTPUT=$output" -P "$PSScriptRoot/stage-runtime.cmake"
if ($LASTEXITCODE -ne 0) { throw 'Runtime dependency staging failed.' }
$commit = & git -C $root rev-parse HEAD
$sdk = & git -C "$root/external/ps3recomp" rev-parse HEAD
$files = @{}
foreach ($file in Get-ChildItem -LiteralPath $output -File) { $files[$file.Name] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant() }
$info = @{ version = (Get-Content "$root/VERSION").Trim(); commit = $commit; sdk = $sdk; files = $files }
[IO.File]::WriteAllText("$output/build-info.json", ($info | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
