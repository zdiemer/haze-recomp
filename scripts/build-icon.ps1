param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][string]$Resource,
    [string]$FallbackSource
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing

function Write-Changed([string]$Path,[byte[]]$Bytes) {
    $Path=[IO.Path]::GetFullPath($Path)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    if ([IO.File]::Exists($Path) -and [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) -eq [Convert]::ToBase64String($Bytes)) { return }
    $temporary=$Path+'.tmp.'+$PID
    try {
        $file=[IO.File]::Open($temporary,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $file.Write($Bytes,0,$Bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
        else { [IO.File]::Move($temporary,$Path) }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Read-Artwork([string]$Path,[string]$Label) {
    if (-not [IO.File]::Exists($Path)) { throw "$Label is missing" }
    if ((Get-Item -LiteralPath $Path).Length -gt 8MB) { throw "$Label exceeds 8 MiB" }
    $inputStream=[IO.File]::OpenRead($Path)
    try {
        $decoded=[Drawing.Image]::FromStream($inputStream,$false,$true)
        try {
            if ($decoded.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid -or
                $decoded.Width -gt 4096 -or $decoded.Height -gt 4096) { throw "Unsupported $Label image" }
            return [Drawing.Bitmap]::new($decoded)
        } finally { $decoded.Dispose() }
    } finally { $inputStream.Dispose() }
}

$art=$null
try {
    $art=Read-Artwork $Source 'ICON0.PNG'
} catch {
    Write-Warning "Local Haze icon unavailable ($($_.Exception.Message))."
    if ($FallbackSource) {
        try { $art=Read-Artwork $FallbackSource 'Bundled Haze logo' }
        catch { Write-Warning "Bundled Haze logo unavailable ($($_.Exception.Message))." }
    }
    if (-not $art) {
        Write-Warning 'Using the Windows application icon.'
        $art=[Drawing.SystemIcons]::Application.ToBitmap()
    }
}

try {
    $sizes=@(16,24,32,48,64,128,256)
    $layers=[Collections.Generic.List[byte[]]]::new()
    foreach ($size in $sizes) {
        $bitmap=[Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        $attributes=[Drawing.Imaging.ImageAttributes]::new()
        $encoded=[IO.MemoryStream]::new()
        try {
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality=[Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $attributes.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY)
            $scale=[Math]::Min($size/$art.Width,$size/$art.Height)
            $width=[Math]::Max(1,[int][Math]::Round($art.Width*$scale))
            $height=[Math]::Max(1,[int][Math]::Round($art.Height*$scale))
            $rect=[Drawing.Rectangle]::new([int][Math]::Floor(($size-$width)/2),[int][Math]::Floor(($size-$height)/2),$width,$height)
            $graphics.DrawImage($art,$rect,0,0,$art.Width,$art.Height,[Drawing.GraphicsUnit]::Pixel,$attributes)
            $bitmap.Save($encoded,[Drawing.Imaging.ImageFormat]::Png)
            $layers.Add($encoded.ToArray())
        } finally { $encoded.Dispose(); $attributes.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
    }
    $buffer=[IO.MemoryStream]::new()
    $writer=[IO.BinaryWriter]::new($buffer)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
        $offset=6+16*$sizes.Count
        for ($i=0;$i -lt $sizes.Count;$i++) {
            $dimension=if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$layers[$i].Length); $writer.Write([uint32]$offset)
            $offset+=$layers[$i].Length
        }
        foreach ($layer in $layers) { $writer.Write($layer) }
        $writer.Flush(); Write-Changed $Output $buffer.ToArray()
    } finally { $writer.Dispose(); $buffer.Dispose() }
    $iconPath=[IO.Path]::GetFullPath($Output).Replace('\','/').Replace('"','\"')
    $rc="#pragma code_page(65001)`n101 ICON DISCARDABLE `"$iconPath`"`n"
    Write-Changed $Resource ([Text.UTF8Encoding]::new($false).GetBytes($rc))
} finally { $art.Dispose() }
