param([string]$Iso, [string]$Firmware, [string]$DataDirectory, [string]$InstallDirectory, [switch]$DownloadFirmware, [switch]$NoGui)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$install = if ($InstallDirectory) { (Resolve-Path -LiteralPath $InstallDirectory).Path } else { $root }
. "$PSScriptRoot/setup-common.ps1"
function Select-Input([string]$Title, [string]$Filter) {
    if ($NoGui) { throw "Missing input: $Title" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = $Title; $dialog.Filter = $Filter
        if ($dialog.ShowDialog() -ne 'OK') { throw 'Setup cancelled.' }
        return $dialog.FileName
    } finally { $dialog.Dispose() }
}
try {
    if (-not $Iso) { $Iso = Select-Input 'Choose the decrypted Haze USA disc ISO' 'PS3 disc ISO (*.iso)|*.iso' }
    $Iso = (Resolve-Path -LiteralPath $Iso).Path
    if (-not $DataDirectory) { $DataDirectory = Join-Path $install 'local/user-data' }
    $data = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DataDirectory)
    if ($data -match '[\r\n]') { throw 'The data path must not contain a newline.' }
    $attempt = Join-Path $data ('setup-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $attempt -Force | Out-Null
    if (-not $Firmware) {
        if (-not $DownloadFirmware -and -not $NoGui) {
            Add-Type -AssemblyName System.Windows.Forms
            $choice = [Windows.Forms.MessageBox]::Show('Download PS3 firmware directly from Sony? Choose No to use a local PS3UPDAT.PUP file.', 'Haze first-run setup', 'YesNoCancel')
            if ($choice -eq 'Cancel') { throw 'Setup cancelled.' }
            $DownloadFirmware = $choice -eq 'Yes'
        }
        if ($DownloadFirmware) {
            $lock = Get-Content "$root/tools/firmware.json" -Raw | ConvertFrom-Json
            $Firmware = Join-Path $attempt 'PS3UPDAT.PUP'
            Write-Output "Downloading PS3 firmware $($lock.version) from Sony..."
            # Sony's HTTPS support page links this HTTP CDN URL. The release
            # pins SHA-256; reject errors/tampering before running the helper.
            Invoke-WebRequest -UseBasicParsing -Uri $lock.url -OutFile "$Firmware.partial" -TimeoutSec 600
            if ((Get-FileHash -LiteralPath "$Firmware.partial").Hash -ne $lock.sha256) { throw 'Sony firmware checksum mismatch. No firmware was installed.' }
            Move-Item -LiteralPath "$Firmware.partial" -Destination $Firmware
        } else { $Firmware = Select-Input 'Choose your PS3 firmware update' 'PS3 firmware (*.pup)|*.pup' }
    }
    $Firmware = (Resolve-Path -LiteralPath $Firmware).Path
    $stream = [IO.File]::OpenRead($Firmware)
    try { $magic = New-Object byte[] 8; $read = $stream.Read($magic,0,8) } finally { $stream.Dispose() }
    if ($read -ne 8 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'SCEUF' + [char]0 + [char]0 + [char]0) { throw 'Invalid firmware: choose a PS3 PS3UPDAT.PUP file.' }
    $tools = Join-Path $root 'tools'
    $sevenZip = Join-Path $tools '7zip/7z.exe'
    $rpcs3 = Join-Path $attempt 'rpcs3'
    if (-not (Test-Path -LiteralPath $sevenZip)) { throw 'Bundled 7-Zip is missing. Reinstall or re-extract the complete release ZIP.' }
    Write-Output 'Downloading the verified setup helper from RPCS3...'
    $helper = (Get-Content "$tools/release-tools.json" -Raw | ConvertFrom-Json).rpcs3
    $helperArchive = Join-Path $attempt 'rpcs3.7z'
    Receive-HazeDownload $helper.url $helper.sha256 $helperArchive
    Invoke-HazeHelper $sevenZip @('x',$helperArchive,"-o$rpcs3",'-y','-bsp0') $attempt "$attempt/helper-extract"
    if (-not (Test-Path -LiteralPath "$rpcs3/rpcs3.exe")) { throw 'RPCS3 download did not contain its executable.' }
    # The helper uses the same app-local CRT as the game, without a separate
    # Visual C++ redistributable installation or changes to another RPCS3 copy.
    Get-ChildItem -LiteralPath $install -Filter '*.dll' | Where-Object {
        $_.Name -match '^(msvcp|vcruntime|concrt|vccorlib)'
    } | Copy-Item -Destination $rpcs3
    # Override inherited RPCS3_CONFIG_DIR and any installed RPCS3 configuration.
    New-Item -ItemType Directory -Path "$rpcs3/portable/log" -Force | Out-Null
    $flash = "$rpcs3/portable/dev_flash"
    $disc = Join-Path $attempt 'disc'
    Write-Output 'Extracting disc files...'
    Invoke-HazeHelper $sevenZip @('x','-tIso',$Iso,'PS3_GAME/*',"-o$disc",'-y','-bsp0') $attempt "$attempt/extract" 1800
    Assert-HazeDisc $disc
    Write-Output 'Decrypting the supported executable...'
    Invoke-HazeHelper "$rpcs3/rpcs3.exe" @('--decrypt',"$disc/PS3_GAME/USRDIR/EBOOT.BIN",'--stdout','--stderr') $rpcs3 "$attempt/decrypt"
    Assert-HazeElf "$disc/PS3_GAME/USRDIR/EBOOT.ELF"
    Write-Output 'Installing user-supplied firmware...'
    # This pinned RPCS3 build can fail-fast during headless shutdown *after*
    # successful extraction. Accept that one status only with exact decoder
    # hashes below; missing, corrupt and unsupported firmware still fail closed.
    Invoke-HazeHelper "$rpcs3/rpcs3.exe" @('--headless','--installfw',$Firmware,'--stdout','--stderr') $rpcs3 "$attempt/firmware" 600 @(0,-1073740791)
    $decoders = @{
        'flashATRAC.pic' = '1edf54e1323dd736b71958ca56307d536dedc7c05ccb0dd3b2a3be1af5b50e30'
        'flashMP3.pic' = 'adbd993483a41f53c994e080665fb5fbf36af783956d0e4ded30057b8b4e894a'
    }
    foreach ($file in $decoders.Keys) {
        if (-not (Test-Path -LiteralPath "$flash/sys/external/$file" -PathType Leaf) -or
            (Get-FileHash -LiteralPath "$flash/sys/external/$file").Hash -ne $decoders[$file]) {
            throw "Unsupported or incomplete firmware decoder: $file. Use the supported Sony download. See $attempt/firmware.*.log"
        }
    }
    # Configuration is switched only after all preparation succeeds. Existing
    # saves and customized launcher settings are retained by the INI API.
    Add-Type @'
using System.Runtime.InteropServices;
public static class HazeSetupIni {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool WritePrivateProfileString(string section, string key, string value, string path);
}
'@
    $config = Join-Path $install 'launcher.ini'
    $pending = "$config.setup-$([Guid]::NewGuid().ToString('N'))"
    $contents = if (Test-Path -LiteralPath $config) { [IO.File]::ReadAllText($config, [Text.Encoding]::UTF8) } else { "[Launcher]`r`n" }
    [IO.File]::WriteAllText($pending, $contents, [Text.Encoding]::Unicode)
    foreach ($setting in @(@('DiscDirectory',$disc), @('Firmware',$flash))) {
        if (-not [HazeSetupIni]::WritePrivateProfileString('Launcher',$setting[0],$setting[1],$pending)) { throw 'Cannot write launcher configuration.' }
    }
    # Use UTF-16 for the Windows INI API, then the launcher's UTF-8 format.
    $contents = [IO.File]::ReadAllText($pending, [Text.Encoding]::Unicode)
    [IO.File]::WriteAllText($pending, $contents, [Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $config) { [IO.File]::Replace($pending,$config,"$config.before-setup") }
    else { [IO.File]::Move($pending,$config) }
    Write-Output "Setup complete. Launch HazeLauncher.exe. Logs and prepared files: $attempt"
} catch {
    Write-Error $_ -ErrorAction Continue
    if (-not $NoGui) {
        Add-Type -AssemblyName System.Windows.Forms
        [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Haze setup failed') | Out-Null
    }
    exit 1
}
