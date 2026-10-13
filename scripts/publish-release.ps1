param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$Notes,
    [Parameter(Mandatory)][string]$SourceArchive
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = (Get-Content "$root/VERSION").Trim()
$tag = "v$version"
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read release commit.' }
$approval = Get-Content -LiteralPath $Evidence -Raw | ConvertFrom-Json
if ($approval.version -ne $version -or $approval.commit -ne $commit) { throw 'Evidence must match this release commit and version.' }
foreach ($gate in @('asset_free_ci','local_game_tests','fresh_setup_sony_download','offline_setup','clean_windows_startup','gameplay_save_load','upgrade_repair_uninstall','redistribution_review')) {
    if ($approval.gates.$gate.passed -ne $true -or -not $approval.gates.$gate.evidence) { throw "Incomplete release gate: $gate" }
}
if (-not (Get-Content -LiteralPath $Notes -Raw).Trim()) { throw 'Release notes are required.' }
$files = @()
foreach ($extension in @('msi','zip')) {
    $name = "HazeRecomp-$version-windows-x64.$extension"
    $file = Join-Path $Directory $name
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($approval.artifacts.$name -ne $hash) { throw "Acceptance evidence does not match $name" }
    $files += $file
}
$sourceHash = (Get-FileHash -LiteralPath $SourceArchive).Hash.ToLowerInvariant()
if ($approval.source_sha256 -ne $sourceHash) { throw 'Source archive does not match redistribution review.' }
$files += $SourceArchive
$checksums = Join-Path $Directory 'SHA256SUMS.txt'
$lines = foreach ($file in $files) { '{0}  {1}' -f (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant(),[IO.Path]::GetFileName($file) }
[IO.File]::WriteAllText($checksums, ($lines -join "`n")+"`n", [Text.Encoding]::ASCII)
$remote = & git -C $root ls-remote origin "refs/tags/$tag" "refs/tags/$tag^{}"
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify remote tag.' }
$peeled = $remote | Where-Object { $_ -match '\^\{\}$' }
$target = if ($peeled) { ($peeled -split '\s+')[0] } elseif ($remote) { ($remote -split '\s+')[0] } else { '' }
if ($target -ne $commit) { throw "Push $tag pointing at $commit before creating a release." }
& gh release create $tag @files $checksums $Evidence --repo zdiemer/haze-recomp --verify-tag --draft --prerelease --title "Haze Recomp $tag (experimental)" --notes-file $Notes
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed; inspect GitHub before retrying.' }
