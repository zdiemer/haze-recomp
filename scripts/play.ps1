param(
    [string]$SharedRoot,
    [ValidateSet('Auto','KeyboardMouse','Controller')][string]$InputMode = 'Auto',
    [string]$ExecutablePath,
    [string]$GraphicsConfig,
    [ValidatePattern('^[1-9][0-9]{0,3}x[1-9][0-9]{0,3}$')]
    [ValidateScript({ $dimensions = $_ -split 'x'; [int]$dimensions[0] -le 8192 -and [int]$dimensions[1] -le 8192 })]
    [string]$RenderResolution
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$assets = if ($SharedRoot) { (Resolve-Path -LiteralPath $SharedRoot).Path } else { $root }
$exe = if ($ExecutablePath) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExecutablePath) } else { Join-Path $root 'build\haze\HazeRecomp.exe' }
$elf = Join-Path $assets 'game\disc\PS3_GAME\USRDIR\EBOOT.ELF'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build the native executable first with scripts/build.ps1.' }
if (-not (Test-Path -LiteralPath $elf)) { throw 'The local decrypted Haze executable is missing.' }
$session = Join-Path $root ('local\sessions\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $session -Force | Out-Null
$shaderCache = Join-Path $root 'local\cache\rsx-shaders'
New-Item -ItemType Directory -Path $shaderCache -Force | Out-Null
$settings = @{
    PS3_VFS_ROOT = (Join-Path $assets 'game\disc')
    PS3_DEV_FLASH = $(if ($env:PS3_DEV_FLASH) { $env:PS3_DEV_FLASH } else { Join-Path $assets 'local\tools\rpcs3\dev_flash' })
    PS3_NET_ONLINE = '1'
    YDKJ_NO_CRI_CTX = '1'
    PS3_SAVEDATA_ROOT = $(if ($env:PS3_SAVEDATA_ROOT) { $env:PS3_SAVEDATA_ROOT } else { Join-Path $root 'local\state\savedata' })
    PS3_HLE_UNRESOLVED = 'fail'
    PS3_VERBOSE = '0'
    RSX_LIVE_DRAW = '1'
    RSX_RENDER_RESOLUTION = $(if ($RenderResolution) { $RenderResolution } else { $env:RSX_RENDER_RESOLUTION })
    RSX_SHADER_CACHE_DIR = $(if ($env:RSX_SHADER_CACHE_DIR) { $env:RSX_SHADER_CACHE_DIR } else { $shaderCache })
    HAZE_INPUT = $(if ($InputMode -eq 'Controller') { 'controller' } elseif ($InputMode -eq 'KeyboardMouse') { 'keyboard' } else { 'auto' })
    HAZE_INPUT_CONFIG = $(if ($env:HAZE_INPUT_CONFIG) { $env:HAZE_INPUT_CONFIG } else { Join-Path $root 'config\input.ini' })
    HAZE_GRAPHICS_CONFIG = $(if ($GraphicsConfig) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GraphicsConfig) } elseif ($env:HAZE_GRAPHICS_CONFIG) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($env:HAZE_GRAPHICS_CONFIG) } elseif (Test-Path -LiteralPath (Join-Path $root 'config\graphics.ini')) { Join-Path $root 'config\graphics.ini' } else { $null })
}
$old = @{}
foreach ($key in $settings.Keys) {
    $old[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process')
}
try {
    Write-Host 'Movies play by default. Automatic logo/attract skipping is optional in input.ini [Startup].'
    Write-Host 'Current Cross/confirm binding skips manually. Default controls: Enter starts; Space confirms; arrows navigate.'
    Write-Host 'WASD moves. Gameplay captures the mouse automatically; F1 releases it.'
    Write-Host 'Escape pauses. Menus use the pointer: left click selects, right click goes back.'
    Write-Host "Session logs: $session"
    # This is the interactive game launcher; the separate run.ps1 is bounded.
    $game = Start-Process -FilePath $exe -ArgumentList ('"{0}"' -f $elf) -WorkingDirectory $session -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $session 'game.stdout.log') -RedirectStandardError (Join-Path $session 'game.stderr.log')
    $null = $game.Handle
    $game.WaitForExit()
    $game.Refresh()
    if ($game.ExitCode -ne 0) { throw "Native game exited with code $($game.ExitCode). See $session" }
} finally {
    foreach ($key in $settings.Keys) { [Environment]::SetEnvironmentVariable($key, $old[$key], 'Process') }
}
