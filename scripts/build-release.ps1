param(
    [string]$ProjectRoot,
    [Parameter(Mandatory)][string]$Inputs,
    [string]$Python = 'python',
    [string]$MsysRoot = 'C:/msys64',
    [int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
$tooling = Split-Path $PSScriptRoot -Parent
if (-not $ProjectRoot) { $ProjectRoot = $tooling }
$project = (Resolve-Path -LiteralPath $ProjectRoot).Path
$inputsPath = (Resolve-Path -LiteralPath $Inputs).Path
$work = "$project/build/release"
if (Test-Path -LiteralPath $work) { throw 'Use a fresh checkout/build directory for a release.' }
$lock = Get-Content "$tooling/config/build-inputs.json" -Raw | ConvertFrom-Json
$inputCommit = & git -C $inputsPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $inputCommit -ne $lock.commit) { throw 'Private build inputs do not match the lock.' }
$sdk = & git -C "$project/external/ps3recomp" rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $sdk -ne $lock.sdk) { throw 'SDK does not match the validated private input snapshot.' }
$gitlink = ((& git -C $project ls-tree HEAD external/ps3recomp) -split '\s+')[2]
if ($gitlink -ne $sdk) { throw 'SDK differs from the release commit.' }
if (& git -C $project status --porcelain) { throw 'Release checkout is dirty.' }
if (& git -C "$project/external/ps3recomp" status --porcelain) { throw 'SDK checkout is dirty.' }
New-Item -ItemType Directory -Force "$work/logs" | Out-Null
$shell = (Get-Process -Id $PID).Path
function Run-ReleaseStep([string]$Name, [string]$Exe, [string[]]$Arguments) {
    Write-Output "Release: $Name"
    # Compiler diagnostics may contain private source excerpts. Keep these
    # local; never upload the logs or emit them into a public Actions job.
    try {
        $ErrorActionPreference = 'Continue'
        & $Exe @Arguments *> "$work/logs/$Name.log"
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = 'Stop' }
    if ($code -ne 0) { throw "$Name failed (exit $code). Logs remain on the build machine." }
}
Run-ReleaseStep 'verify-inputs' $Python @("$tooling/scripts/build_inputs.py", '--verify', $inputsPath)
Run-ReleaseStep 'sources' $Python @("$tooling/scripts/release_sources.py", '--fetch', "$work/sources")
Run-ReleaseStep 'ffmpeg' $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$tooling/scripts/build-ffmpeg.ps1",
    '-Source',"$work/sources/ffmpeg",'-OutputDirectory',"$work/ffmpeg",'-MsysRoot',$MsysRoot)
Run-ReleaseStep 'game' $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$project/scripts/build.ps1",
    '-Configuration','Release','-SharedRoot',$inputsPath,'-Python',$Python,'-Jobs',"$Jobs",
    '-SDL3Root',"$work/sources/sdl",'-FFmpegRoot',"$work/ffmpeg")
Run-ReleaseStep 'launcher' $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$project/scripts/build-launcher.ps1",
    '-Configuration','Release','-Test')
Run-ReleaseStep 'tools' $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$tooling/scripts/prepare-release-tools.ps1",
    '-OutputDirectory',"$work/tools")
Run-ReleaseStep 'stage' $shell @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$project/scripts/stage-runtime.ps1",
    '-Game',"$project/build/haze/HazeRecomp.exe",'-Launcher',"$project/build/launcher/bin/HazeLauncher.exe",
    '-OutputDirectory',"$work/runtime")
Run-ReleaseStep 'licenses' $Python @("$tooling/scripts/release_sources.py",'--sources',"$work/sources",
    '--project',$project,'--ffmpeg',"$work/ffmpeg",'--output',"$work/redistribution")
Run-ReleaseStep 'wix' 'dotnet' @('tool','install','wix','--version','6.0.2','--tool-path',"$work/wix")
Run-ReleaseStep 'package' $Python @("$tooling/scripts/release.py",'--project-root',$project,
    '--runtime',"$work/runtime",'--tools',"$work/tools",'--licenses',"$work/redistribution/licenses",
    '--notes',"$tooling/docs/release-notes.txt",'--output',"$work/package",'--wix',"$work/wix/wix.exe")
Run-ReleaseStep 'deliverables' $Python @("$tooling/scripts/release_artifacts.py",'prepare',
    '--project',$project,'--work',$work,'--inputs-commit',$inputCommit)
Write-Output "Release artifacts ready: $work/artifacts"
