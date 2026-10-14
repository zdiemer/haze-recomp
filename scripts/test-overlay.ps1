param([switch]$Gpu)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
$sdk=Join-Path $root 'external/ps3recomp'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$environment=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
if ($env:HAZE_LLVM_ROOT) { $clang=Join-Path $env:HAZE_LLVM_ROOT 'bin/clang-cl.exe' }
New-Item -ItemType Directory -Path "$root/local" -Force | Out-Null
$ErrorActionPreference='Continue'
foreach ($test in @('test_haze_overlay','test_haze_graphics','test_haze_video_backend','test_haze_menu','test_haze_input','test_haze_controls')) {
    & $clang /nologo /W0 /O2 /std:c++20 /EHsc "/I$root/tests/fixtures/camera" "/I$root/tests/fixtures/video" "/I$sdk/include" "/I$sdk/libs/input" "/I$sdk/libs/codec" "/I$sdk/libs/video" "/I$sdk/libs/system" "/Fe$root/local/$test.exe" "/Fo$root/local/$test.obj" "$root/tests/$test.cpp" /link user32.lib gdi32.lib comdlg32.lib
    if ($LASTEXITCODE -ne 0) { throw "$test compilation failed." }
    & "$root/local/$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$test failed." }
}
if ($Gpu) {
    & $clang /nologo /W0 /O2 /std:c11 "/Fe$root/local/test_haze_overlay_d3d12.exe" "/Fo$root/local/test_haze_overlay_d3d12.obj" "$root/tests/test_haze_overlay_d3d12.c" /link d3d12.lib d3dcompiler.lib dxguid.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'D3D12 overlay test compilation failed.' }
    & "$root/local/test_haze_overlay_d3d12.exe"
    if ($LASTEXITCODE -ne 0) { throw 'D3D12 overlay readback test failed.' }
}
exit 0
