param([ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag='current',
      [ValidateRange(0,3)][int]$Modes=3)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$install=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $install){throw 'Visual Studio C++ build tools are required.'}
$vcvars=Join-Path $install 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if($LASTEXITCODE -ne 0){throw 'Could not initialize Visual Studio environment.'}
foreach($line in $environment){
    if($line -match '^([^=]+)=(.*)$'){
        [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
    }
}
$clang=Join-Path $install 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$sdk=Join-Path $root 'external/ps3recomp'
$output=Join-Path $root "local/benchmark_vertex_fetch_$Tag.exe"
$library=Join-Path $root 'build/haze/ps3recomp_sdk/ps3recomp_runtime.lib'
if(-not(Test-Path -LiteralPath $library)){throw 'Build the native runtime first.'}
$objects=Join-Path $root "local/vertex-benchmark-$Tag"
New-Item -ItemType Directory -Path $objects -Force | Out-Null
# Compile the current kernel directly. The existing runtime library supplies
# unchanged dispatch dependencies; its compact object is not selected.
& $clang /nologo /W3 /O2 /MD /Gy /Gw /clang:-msse4.1 "/I$sdk/include" "/I$sdk/libs/video" "/Fe$output" "/Fo$objects/" "$root/tests/benchmark_vertex_fetch.c" "$sdk/libs/video/rsx_vertex_compact.c" /link /OPT:REF $library
if($LASTEXITCODE -ne 0){throw 'Vertex benchmark compilation failed.'}
& $output $Modes
if($LASTEXITCODE -ne 0){throw 'Vertex benchmark failed.'}
