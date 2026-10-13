param(
    [string]$OutputDirectory,
    [string]$SevenZip = 'C:/Program Files/7-Zip/7z.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $OutputDirectory) { $OutputDirectory = "$root/build/release-tools" }
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory; never bundle an existing RPCS3 installation.' }
if (-not (Test-Path -LiteralPath $SevenZip -PathType Leaf)) { throw 'The release builder requires 7-Zip to unpack the pinned tools.' }
$lock = Get-Content "$root/config/release-tools.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $output | Out-Null
foreach ($name in @('rpcs3', '7zip')) {
    $item = $lock.$name
    $archive = Join-Path $output ([IO.Path]::GetFileName($item.url))
    Invoke-WebRequest -UseBasicParsing -Uri $item.url -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $item.sha256) { throw "Checksum mismatch: $name" }
    & $SevenZip x $archive "-o$output/$name" -y -bso0 -bsp0
    if ($LASTEXITCODE -ne 0) { throw "Cannot unpack $name" }
    # Retain the exact verified upstream archive as well as its unmodified contents.
}
Copy-Item -LiteralPath "$root/config/release-tools.json" -Destination "$output/release-tools.json"
$files = @{}
foreach ($name in @('rpcs3', '7zip')) {
    $files[$name] = @{}
    $base = (Resolve-Path -LiteralPath "$output/$name").Path
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File) {
        $relative = $file.FullName.Substring($base.Length+1).Replace('\','/')
        $files[$name][$relative] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    }
}
[IO.File]::WriteAllText("$output/files.json", ($files | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Output "Verified tools prepared at $output"
