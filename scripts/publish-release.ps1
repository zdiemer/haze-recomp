param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$Notes,
    [string]$Python = 'python',
    [string]$Repository = 'zdiemer/haze-recomp',
    [long]$ResumeDraftId = 0,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
if ($Tag -notmatch '^v\d+\.\d+\.\d+$' -or $Commit -notmatch '^[a-f0-9]{40}$') { throw 'Invalid release target.' }
if ($ResumeDraftId -lt 0 -or ($VerifyOnly -and -not $ResumeDraftId)) { throw 'VerifyOnly requires a positive ResumeDraftId.' }
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
function Find-TaggedRelease {
    # The release-by-tag endpoint only resolves published releases. Listing with
    # push access includes drafts; emit one JSON object per line across pages.
    $lines = @(& gh api "repos/$Repository/releases?per_page=100" --paginate --jq '.[] | @json')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot check existing releases.' }
    $matches = @($lines | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object tag_name -EQ $Tag)
    if ($matches.Count -gt 1) { throw 'Multiple releases match the tag; inspect them before retrying.' }
    if ($matches.Count -eq 1) { return $matches[0] }
}
$release = Find-TaggedRelease
if ($release) {
    if (-not $ResumeDraftId) { throw 'Release already exists. Inspect it before retrying; published files are never overwritten.' }
    if ($release.id -ne $ResumeDraftId -or $release.draft -ne $true) { throw 'Resume target is not the requested unpublished draft.' }
} elseif ($ResumeDraftId) { throw 'Requested draft does not exist for this tag.' }
$files = @(Get-ChildItem -LiteralPath $Directory -File | ForEach-Object FullName)
# Keep partial uploads private. Only reveal the release after every asset exists.
if (-not $release) {
    & gh release create $Tag @files --repo $Repository --verify-tag --draft --title "Haze Recomp $Tag" --notes-file $Notes
    if ($LASTEXITCODE -ne 0) { throw 'Draft upload failed. Inspect the draft before retrying.' }
    $release = Find-TaggedRelease
}
if (-not $release -or $release.id -le 0) { throw 'Cannot locate uploaded draft.' }
$endpoint = "repos/$Repository/releases/$($release.id)"
$uploaded = & gh api $endpoint | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify uploaded release assets.' }
if ($uploaded.id -ne $release.id -or $uploaded.tag_name -ne $Tag -or $uploaded.draft -ne $true) { throw 'Release identity or draft state changed; refusing publication.' }
$expected = @($files | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object)
$actual = @($uploaded.assets | ForEach-Object name | Sort-Object)
if (Compare-Object $expected $actual) { throw 'Uploaded asset set differs; release remains a draft.' }
foreach ($file in $files) {
    $asset = $uploaded.assets | Where-Object name -EQ ([IO.Path]::GetFileName($file))
    if ($asset.size -ne (Get-Item -LiteralPath $file).Length) { throw 'Uploaded size differs; release remains a draft.' }
    $expectedDigest = 'sha256:' + (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($asset.digest -ne $expectedDigest) { throw 'Uploaded checksum differs; release remains a draft.' }
}
if ($VerifyOnly) { Write-Output "Verified draft $($uploaded.id) for $Tag; no changes made."; return }
$published = & gh api $endpoint --method PATCH --field 'draft=false' | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Publishing failed; inspect the existing draft.' }
if ($published.id -ne $uploaded.id -or $published.tag_name -ne $Tag -or $published.draft -ne $false) { throw 'Unexpected publication response; inspect the release.' }
Write-Output $published.html_url
