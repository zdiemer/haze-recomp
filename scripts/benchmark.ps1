<#
.SYNOPSIS
Build and run a native microbenchmark without launching the game.
.EXAMPLE
.\scripts\benchmark.ps1 spu-abi -Heap
.EXAMPLE
.\scripts\benchmark.ps1 vertex-fetch -Modes 3 -Tag current
.EXAMPLE
.\scripts\benchmark.ps1 write-watch
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position=0)]
    [ValidateSet('spu-abi', 'vertex-fetch', 'write-watch')][string]$Benchmark,
    [string]$SdkRoot,
    [switch]$Heap,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag = 'current',
    [ValidateRange(0,3)][int]$Modes = 3,
    [string]$RuntimeLibrary
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Heap -and $Benchmark -ne 'spu-abi') { throw '-Heap applies only to spu-abi.' }
foreach ($option in @('Tag', 'Modes', 'RuntimeLibrary')) {
    if ($PSBoundParameters.ContainsKey($option) -and $Benchmark -ne 'vertex-fetch') {
        throw "-$option applies only to vertex-fetch."
    }
}
$sdk = if ($SdkRoot) { (Resolve-Path -LiteralPath $SdkRoot).Path } else { Join-Path $root 'external/ps3recomp' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $install 'VC/Auxiliary/Build/vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio environment.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$clang = Join-Path $install 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$directory = Join-Path $root "local/benchmarks/$Benchmark"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$output = Join-Path $directory 'benchmark.exe'
$runArguments = @()
switch ($Benchmark) {
    'spu-abi' {
        $compileArguments = @('/W0', '/O2', '/clang:-msse4.1', "/I$sdk/runtime/spu", "/I$sdk/include",
            "$root/tests/benchmark_spu_abi.c")
        if ($Heap) { $runArguments = @('heap') }
    }
    'vertex-fetch' {
        if (-not $RuntimeLibrary) { $RuntimeLibrary = Join-Path $root 'build/haze/ps3recomp_sdk/ps3recomp_runtime.lib' }
        if (-not (Test-Path -LiteralPath $RuntimeLibrary)) { throw 'Build the native runtime first, or pass -RuntimeLibrary.' }
        $output = Join-Path $directory "benchmark_$Tag.exe"
        # Compile the current kernel; the library supplies unchanged dependencies.
        $compileArguments = @('/W3', '/O2', '/MD', '/Gy', '/Gw', '/clang:-msse4.1',
            "/I$sdk/include", "/I$sdk/libs/video", "$root/tests/benchmark_vertex_fetch.c",
            "$sdk/libs/video/rsx_vertex_compact.c", '/link', '/OPT:REF', $RuntimeLibrary)
        $runArguments = @($Modes)
    }
    'write-watch' {
        $compileArguments = @('/W3', '/O2', '/MD', '/EHsc', '/std:c++17',
            "/I$sdk/libs/video", "$root/tests/benchmark_write_watch.cpp")
    }
}
& $clang /nologo "/Fe$output" "/Fo$directory/" @compileArguments
if ($LASTEXITCODE -ne 0) { throw "$Benchmark compilation failed." }
& $output @runArguments
if ($LASTEXITCODE -ne 0) { throw "$Benchmark failed." }
