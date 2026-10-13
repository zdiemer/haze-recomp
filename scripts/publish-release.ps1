param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$Notes,
    [string]$Python = 'python',
    [string]$Repository = 'zdiemer/haze-recomp'
)
$ErrorActionPreference = 'Stop'
if ($Tag -notmatch '^v\d+\.\d+\.\d+$' -or $Commit -notmatch '^[a-f0-9]{40}$') { throw 'Invalid release target.' }
$root = Split-Path $PSScriptRoot -Parent
& $Python "$root/scripts/release_artifacts.py" verify --directory $Directory --tag $Tag --commit $Commit
if ($LASTEXITCODE -ne 0) { throw 'Release artifact verification failed.' }
$ref = & gh api "repos/$Repository/git/ref/tags/$Tag" | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Release tag does not exist.' }
$object = $ref.object
while ($object.type -eq 'tag') {
    $annotated = & gh api "repos/$Repository/git/tags/$($object.sha)" | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve annotated release tag.' }
    $object = $annotated.object
}
if ($object.type -ne 'commit' -or $object.sha -ne $Commit) { throw 'Remote tag does not match the binaries.' }
$releases = & gh api "repos/$Repository/releases?per_page=100" --paginate | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot check existing releases.' }
$existing = @($releases | Where-Object tag_name -EQ $Tag)
if ($existing.Count) { throw 'Release already exists. Inspect it before retrying; published files are never overwritten.' }
$files = @(Get-ChildItem -LiteralPath $Directory -File | ForEach-Object FullName)
# Keep partial uploads private. Only reveal the release after every asset exists.
& gh release create $Tag @files --repo $Repository --verify-tag --draft --title "Haze Recomp $Tag" --notes-file $Notes
if ($LASTEXITCODE -ne 0) { throw 'Draft upload failed. Inspect the draft before retrying.' }
$uploaded = & gh release view $Tag --repo $Repository --json assets | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify uploaded release assets.' }
$expected = @($files | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object)
$actual = @($uploaded.assets | ForEach-Object name | Sort-Object)
if (Compare-Object $expected $actual) { throw 'Uploaded asset set differs; release remains a draft.' }
foreach ($file in $files) {
    $asset = $uploaded.assets | Where-Object name -EQ ([IO.Path]::GetFileName($file))
    if ($asset.size -ne (Get-Item -LiteralPath $file).Length) { throw 'Uploaded size differs; release remains a draft.' }
}
& gh release edit $Tag --repo $Repository --draft=false
if ($LASTEXITCODE -ne 0) { throw 'Publishing failed; inspect the existing draft.' }
