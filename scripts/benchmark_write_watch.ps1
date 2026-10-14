param()
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
$output=Join-Path $root 'local/benchmark_write_watch.exe'
& $clang /nologo /W3 /O2 /MD /EHsc /std:c++17 "/I$root/external/ps3recomp/libs/video" "/Fe$output" "/Fo$root/local/benchmark_write_watch.obj" "$root/tests/benchmark_write_watch.cpp"
if($LASTEXITCODE -ne 0){throw 'Write-watch probe compilation failed.'}
& $output
if($LASTEXITCODE -ne 0){throw 'Write-watch probe failed.'}
