$ErrorActionPreference = 'Stop'
function Receive-HazeDownload([string]$Url, [string]$Sha256, [string]$Destination) {
    if ($Sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid download checksum.' }
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$Destination.partial" -TimeoutSec 600
    if ((Get-FileHash -LiteralPath "$Destination.partial" -Algorithm SHA256).Hash -ne $Sha256) {
        throw 'Setup download checksum mismatch. The downloaded file was not used.'
    }
    Move-Item -LiteralPath "$Destination.partial" -Destination $Destination
}
function Read-HazeSfo([string]$Path) {
    $data = [IO.File]::ReadAllBytes($Path)
    if ($data.Length -lt 20 -or [BitConverter]::ToUInt32($data, 0) -ne 0x46535000) {
        throw 'Unreadable PARAM.SFO. Supply a decrypted PS3 disc ISO, not an encrypted dump.'
    }
    $keys = [long][BitConverter]::ToUInt32($data, 8)
    $values = [long][BitConverter]::ToUInt32($data, 12)
    $count = [long][BitConverter]::ToUInt32($data, 16)
    if ($count -gt 1024 -or 20 + 16*$count -gt $data.Length -or $keys -ge $data.Length -or $values -ge $data.Length) { throw 'Truncated PARAM.SFO.' }
    $result = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $entry = 20 + 16*$i
        $key = $keys + [BitConverter]::ToUInt16($data, $entry)
        $size = [long][BitConverter]::ToUInt32($data, $entry+4)
        $offset = $values + [BitConverter]::ToUInt32($data, $entry+12)
        if ($key -ge $values -or $offset+$size -gt $data.Length -or $size -eq 0) { throw 'Invalid PARAM.SFO bounds.' }
        $end = $key
        while ($end -lt $values -and $data[$end] -ne 0) { $end++ }
        if ($end -eq $values) { throw 'Invalid PARAM.SFO key.' }
        $name = [Text.Encoding]::UTF8.GetString($data, $key, $end-$key)
        if ($result.ContainsKey($name)) { throw 'Duplicate PARAM.SFO key.' }
        $result[$name] = [Text.Encoding]::UTF8.GetString($data, $offset, $size).TrimEnd([char]0)
    }
    return $result
}
function Assert-HazeDisc([string]$Disc) {
    $sfo = Read-HazeSfo (Join-Path $Disc 'PS3_GAME/PARAM.SFO')
    if ($sfo.TITLE_ID -ne 'BLUS30094' -or $sfo.VERSION -ne '01.02' -or $sfo.APP_VER -ne '01.00') {
        throw "Unsupported disc: $($sfo.TITLE_ID), version $($sfo.VERSION), application $($sfo.APP_VER). Requires BLUS30094 01.02 / 01.00."
    }
    $eboot = Join-Path $Disc 'PS3_GAME/USRDIR/EBOOT.BIN'
    if ((Get-FileHash -LiteralPath $eboot -Algorithm SHA256).Hash -ne 'ba7f71740691b0b15829c0cab27a4a384a0128b32d1b759e00526e71a7850f34') {
        throw 'Unsupported EBOOT.BIN. Use the unmodified supported disc, without game updates.'
    }
}
function Assert-HazeElf([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne '07d6dfb04310d0a9fd0d2be554573c28dbf990499b1eb22e5c964e6d762a07bb') {
        throw 'EBOOT decryption failed or produced an unsupported ELF. See the setup logs.'
    }
}
function Invoke-HazeHelper([string]$Exe, [string[]]$Arguments, [string]$WorkingDirectory, [string]$Log, [int]$TimeoutSeconds = 600, [int[]]$AllowedExitCodes = @(0)) {
    # Windows paths cannot contain quotes. Quote every argument, including spaces.
    $quoted = foreach ($arg in $Arguments) {
        if ($arg.Contains('"') -or $arg.EndsWith('\')) { throw 'Unsupported helper argument.' }
        '"' + $arg + '"'
    }
    $process = Start-Process -FilePath $Exe -ArgumentList ($quoted -join ' ') -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Log.stdout.log" -RedirectStandardError "$Log.stderr.log"
    $handle = $process.Handle # Retain the Windows handle so ExitCode survives WaitForExit.
    try {
        if (-not $process.WaitForExit($TimeoutSeconds*1000)) { $process.Kill(); $process.WaitForExit(); throw "Setup helper timed out. See $Log.*.log" }
        $process.WaitForExit()
        [IO.File]::WriteAllText("$Log.exit-code.txt", [string]$process.ExitCode)
        if ($AllowedExitCodes -notcontains $process.ExitCode) { throw "Setup helper failed ($($process.ExitCode)). See $Log.*.log" }
    } finally { $process.Dispose() }
}
