param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio environment.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$clang = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
& $clang /nologo /O2 /LD "/Fe$root/local/native_stackwalk.dll" "/Fo$root/local/native_stackwalk.obj" "$root/scripts/native_stackwalk.c" /link dbghelp.lib "/IMPLIB:$root/local/native_stackwalk.lib"
if ($LASTEXITCODE -ne 0) { throw 'Native stackwalk helper compilation failed.' }
if ($Test) {
    $fixture = Join-Path $root 'local/profiler-stack-oracle'
    New-Item -ItemType Directory -Path $fixture -Force | Out-Null
    & $clang /nologo /O2 /Z7 "/Fe$fixture/HazeRecomp.exe" "/Fo$fixture/oracle.obj" "$root/tests/native_cpu_profiler_fixture.c" /link /DEBUG "/PDB:$fixture/HazeRecomp.pdb"
    if ($LASTEXITCODE -ne 0) { throw 'Native profiler fixture compilation failed.' }
    & python "$root/tests/test_native_cpu_profiler.py"
    if ($LASTEXITCODE -ne 0) { throw 'Native profiler stack oracle failed.' }
}
