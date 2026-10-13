param([string]$Python = 'python', [string]$SDL3Root)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
New-Item -ItemType Directory -Force "$root/local/ci" | Out-Null
Start-Transcript -Path "$root/local/ci/checks.log" -Force
try {
    foreach ($path in @('game/disc', 'generated/ppu', 'generated/spu', 'local/tools/rpcs3/dev_flash')) {
        if (Test-Path -LiteralPath "$root/$path") { throw "Asset-free CI requires a checkout without $path" }
    }
    $expected = ((& git ls-tree HEAD external/ps3recomp) -split '\s+')[2]
    $actual = & git -C external/ps3recomp rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) { throw 'SDK must match the committed gitlink; run git submodule update --init.' }
    if (& git -C external/ps3recomp status --porcelain) { throw 'SDK has local modifications.' }
    & $Python --version
    if ($LASTEXITCODE -ne 0) { throw 'Python is required.' }
    Write-Output "SDK: $actual; runner image: $env:ImageVersion"
    $shell = (Get-Process -Id $PID).Path
    $checks = @(
        @('build.ps1', '-RuntimeOnly', '-Test', '-Python', $Python),
        @('build-launcher.ps1', '-Test'),
        @('test-input.ps1', '-AssetFree', '-Python', $Python),
        @('test-icon.ps1'),
        @('test-steam.ps1')
    )
    foreach ($check in $checks) {
        $name = @($check)[0]
        # Keep a one-argument invocation an array: Windows PowerShell native
        # splatting of a scalar '-Test' does not preserve the switch correctly.
        $arguments = @($check | Select-Object -Skip 1)
        if ($name -eq 'test-input.ps1' -and $SDL3Root) { $arguments += @('-SDL3Root', $SDL3Root) }
        # Transcripts alone omit output from child PowerShell processes.
        # Keep each complete suite log, including native compiler stderr.
        try {
            $ErrorActionPreference = 'Continue'
            & $shell -NoProfile -ExecutionPolicy Bypass -File "$root/scripts/$name" @arguments 2>&1 |
                Tee-Object -FilePath "$root/local/ci/$name.log"
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = 'Stop' }
        if ($code -ne 0) { throw "CI failed: $name" }
    }
    foreach ($test in @('test_thread_priority.py', 'test_setup.py', 'test_release.py', 'test_release_pipeline.py')) {
        try {
            $ErrorActionPreference = 'Continue'
            & $Python "$root/tests/$test" 2>&1 | Tee-Object -FilePath "$root/local/ci/$test.log"
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = 'Stop' }
        if ($code -ne 0) { throw "CI failed: $test" }
    }
    Write-Output 'Local-only: game build, lifted-code guards, disc movie decode/playback, firmware compatibility, gameplay, real devices and clean Windows acceptance. See docs/releasing.md.'
} finally { Stop-Transcript }
