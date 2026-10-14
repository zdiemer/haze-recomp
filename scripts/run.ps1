param(
    [string]$SharedRoot,
    [ValidateRange(1,1800)][int]$Seconds = 60,
    [ValidateRange(0,10000)][int]$TraceCalls = 0,
    [ValidateSet(0,2,4,8,16)][int]$CaptureFrames = 0,
    [ValidateRange(0,1000000)][int]$FrameSkip = 0,
    [ValidateRange(1,1000000)][int]$FrameEvery = 1,
    [ValidateSet('Live','Legacy')][string]$Renderer = 'Live',
    [switch]$Audio,
    [switch]$CaptureAudio,
    [switch]$Milestones,
    [switch]$BootReport,
    [string]$ExecutablePath,
    [string]$GraphicsConfig,
    [switch]$Uncapped,
    [ValidatePattern('^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$')]
    [ValidateScript({ $dimensions = $_ -split 'x'; [int]$dimensions[0] -le 8192 -and [int]$dimensions[1] -le 8192 })]
    [string]$RenderResolution
)
$ErrorActionPreference = 'Stop'
if ($Renderer -eq 'Legacy' -and ($RenderResolution -or $env:RSX_RENDER_RESOLUTION)) {
    throw 'Custom rendering resolutions require the Live renderer.'
}
$root = Split-Path $PSScriptRoot -Parent
$assets = if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
$exe = if ($ExecutablePath) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExecutablePath) } else { Join-Path $root 'build\haze\HazeRecomp.exe' }
$elf = Join-Path $assets 'game\disc\PS3_GAME\USRDIR\EBOOT.ELF'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build the native executable first.' }
if (-not (Test-Path -LiteralPath $elf)) { throw 'The decrypted Haze ELF is missing.' }
$runDir = Join-Path $root ('local\runs\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$shaderCache = Join-Path $assets 'local\cache\rsx-shaders'
New-Item -ItemType Directory -Path $shaderCache -Force | Out-Null
# This is a bounded diagnostic runner, not a claim of working gameplay.
# Keep missing imports visible rather than relying on fabricated success.
$settings = @{
    PS3_VFS_ROOT = (Join-Path $assets 'game\disc')
    PS3_DEV_FLASH = $(if ($env:PS3_DEV_FLASH) { $env:PS3_DEV_FLASH } else { Join-Path $assets 'local\tools\rpcs3\dev_flash' })
    PS3_NET_ONLINE = '1'
    YDKJ_NO_CRI_CTX = '1' # Disable the SDK's unrelated image-22 game-specific patch.
    PS3_SAVEDATA_ROOT = $(if ($env:PS3_SAVEDATA_ROOT) { $env:PS3_SAVEDATA_ROOT } else { Join-Path $root 'local\state\savedata' })
    HAZE_INPUT_CONFIG = $(if ($env:HAZE_INPUT_CONFIG) { $env:HAZE_INPUT_CONFIG } else { Join-Path $root 'config\input.ini' })
    HAZE_GRAPHICS_CONFIG = $(if ($GraphicsConfig) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GraphicsConfig) } elseif ($env:HAZE_GRAPHICS_CONFIG) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($env:HAZE_GRAPHICS_CONFIG) } elseif (Test-Path -LiteralPath (Join-Path $root 'config\graphics.ini')) { Join-Path $root 'config\graphics.ini' } else { $null })
    PS3_HLE_TRACE = [string]$TraceCalls
    PS3_HLE_UNRESOLVED = 'fail'
    # Recurring guest-thread dumps and stream flushes belong to explicit
    # bring-up runs, not ordinary frame-time measurements.
    HAZE_BOOT_REPORT = $(if ($BootReport) { '1' } else { $null })
    # Automated diagnostic runs are silent; retain the real audio pipeline and
    # device timing for representative performance measurements.
    AUDIO_MUTE = $(if ($Audio) { '0' } else { '1' })
    AUDIO_WAV = $(if ($CaptureAudio) { Join-Path $runDir 'audio.f32le' } else { $null })
    PS3_VERBOSE = $(if ($null -ne $env:PS3_VERBOSE) { $env:PS3_VERBOSE } else { '0' })
    RSX_LIVE_DRAW = $(if ($Renderer -eq 'Live') { '1' } else { '0' })
    RSX_RENDER_RESOLUTION = $(if ($RenderResolution) { $RenderResolution } else { $env:RSX_RENDER_RESOLUTION })
    RSX_MAX_FPS = $(if ($Uncapped) { '0' } else { $env:RSX_MAX_FPS })
    RSX_SHADER_CACHE_DIR = $(if ($env:RSX_SHADER_CACHE_DIR) { $env:RSX_SHADER_CACHE_DIR } else { $shaderCache })
    # Milestone checkpoints can perform synchronous I/O on guest threads.
    # Explicit bring-up diagnostic only; keep ordinary performance runs clear.
    PS3_MILESTONE_OUT = $(if ($Milestones) { Join-Path $runDir 'boot-milestones.log' } else { $null })
    CELLMARK_DUMP = $(if ($CaptureFrames -gt 0) { [string]$CaptureFrames } else { $null })
    CELLMARK_DUMP_MINDRAWS = '1'
    CELLMARK_DUMP_DIR = (Join-Path $runDir 'frames')
    CELLMARK_DUMP_SKIP = [string]$FrameSkip
    CELLMARK_DUMP_EVERY = [string]$FrameEvery
    LD_FRAME_DUMP = $(if ($CaptureFrames -gt 0 -and $Renderer -eq 'Live') { Join-Path $runDir 'frames' } else { $null })
    LD_FRAME_DUMP_FROM = [string]$FrameSkip
    LD_FRAME_DUMP_EVERY = [string]$FrameEvery
    LD_FRAME_DUMP_COUNT = [string]$CaptureFrames
    LD_FRAME_DUMP_TRIGGER = $(if ($CaptureFrames -gt 0 -and $Renderer -eq 'Live') { Join-Path $runDir 'capture.request' } else { $null })
}
$old = @{}
foreach ($key in $settings.Keys) {
    $old[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process')
}
New-Item -ItemType Directory -Path (Join-Path $root 'local') -Force | Out-Null
if ($CaptureFrames -gt 0) {
    New-Item -ItemType Directory -Path $settings.CELLMARK_DUMP_DIR -Force | Out-Null
}
try {
    # Shader dumps from the SDK use relative paths. Keep them inside this
    # ignored run directory, along with the logs and captured game frames.
    $p = Start-Process -FilePath $exe -ArgumentList ('"{0}"' -f $elf) -WorkingDirectory $runDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDir 'boot.stdout.log') -RedirectStandardError (Join-Path $runDir 'boot.stderr.log')
    # Windows PowerShell loses ExitCode for redirected children unless the
    # process handle is acquired while the child is still alive.
    $null = $p.Handle
    $ended = $p.WaitForExit($Seconds * 1000)
    if (-not $ended) {
        Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
        if (-not $p.WaitForExit(5000)) { throw 'Timed-out native process did not terminate.' }
    }
    # Record effective file/env settings reported by the native process, rather
    # than only launcher overrides (which are usually empty with an INI).
    $effectiveGraphics = [ordered]@{}
    foreach ($line in (Get-Content -LiteralPath (Join-Path $runDir 'boot.stderr.log') -TotalCount 64)) {
        if ($line -cmatch '^\[graphics\] ([a-z_]+)=(.*)$') { $effectiveGraphics[$matches[1]] = $matches[2] }
    }
    $result = [ordered]@{
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        timeout_seconds = $Seconds
        timed_out = -not $ended
        exit_code = $(if ($ended) { $p.Refresh(); $p.ExitCode } else { $null })
        gameplay_validated = $false
        output_directory = $runDir
        capture_skip = $FrameSkip
        capture_every = $FrameEvery
        renderer = $Renderer
        render_resolution = $(if ($effectiveGraphics.Contains('render_resolution')) { $effectiveGraphics['render_resolution'] } else { $settings.RSX_RENDER_RESOLUTION })
        graphics_config = $settings.HAZE_GRAPHICS_CONFIG
        graphics_settings = $effectiveGraphics
        audio_muted = -not $Audio
        audio_capture = [bool]$CaptureAudio
        milestone_logging = [bool]$Milestones
        boot_report_logging = [bool]$BootReport
        max_fps = $(if ($effectiveGraphics.Contains('frame_limit')) { $effectiveGraphics['frame_limit'] } else { $settings.RSX_MAX_FPS })
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'local\boot-result.json') -Encoding UTF8
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'boot-result.json') -Encoding UTF8
    $result | ConvertTo-Json
} finally {
    foreach ($key in $settings.Keys) { [Environment]::SetEnvironmentVariable($key, $old[$key], 'Process') }
}
