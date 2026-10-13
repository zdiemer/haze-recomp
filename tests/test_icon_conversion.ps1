param([string]$Converter,[string]$Directory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
[IO.Directory]::CreateDirectory($Directory) | Out-Null
$source=Join-Path $Directory 'wide source.png'
$output=Join-Path $Directory 'app.ico'
$resource=Join-Path $Directory 'app.rc'
$sourceImage=[Drawing.Bitmap]::new(80,40)
$drawing=[Drawing.Graphics]::FromImage($sourceImage)
try { $drawing.Clear([Drawing.Color]::Red); $sourceImage.Save($source,[Drawing.Imaging.ImageFormat]::Png) }
finally { $drawing.Dispose(); $sourceImage.Dispose() }
& $Converter -Source $source -Output $output -Resource $resource
$original=[IO.File]::ReadAllBytes($output)
$timestamp=[IO.File]::GetLastWriteTimeUtc($output)
& $Converter -Source $source -Output $output -Resource $resource
if ([IO.File]::GetLastWriteTimeUtc($output) -ne $timestamp) { throw 'Unchanged conversion rewrote its output' }
$stream=[IO.MemoryStream]::new($original)
$reader=[IO.BinaryReader]::new($stream)
try {
    if ($reader.ReadUInt16() -ne 0 -or $reader.ReadUInt16() -ne 1 -or $reader.ReadUInt16() -ne 7) { throw 'Invalid ICO header' }
    $sizes=@(16,24,32,48,64,128,256)
    for ($i=0;$i -lt 7;$i++) {
        $stream.Position=6+16*$i
        $width=$reader.ReadByte(); $height=$reader.ReadByte(); $stream.Position+=6
        $length=$reader.ReadUInt32(); $offset=$reader.ReadUInt32()
        $expected=if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        if ($width -ne $expected -or $height -ne $expected) { throw 'Wrong ICO directory dimensions' }
        $stream.Position=$offset; $png=[IO.MemoryStream]::new($reader.ReadBytes($length))
        $image=[Drawing.Bitmap]::new($png)
        try {
            $size=$sizes[$i]
            if ($image.Width -ne $size -or $image.Height -ne $size) { throw 'Wrong PNG dimensions' }
            if ($image.GetPixel(0,0).A -ne 0 -or $image.GetPixel($size/2,$size/2).R -ne 255) { throw 'Transparency or color changed' }
            $rows=0
            for ($y=0;$y -lt $size;$y++) { if ($image.GetPixel($size/2,$y).A -gt 128) { $rows++ } }
            if ($rows -ne $size/2) { throw 'Source proportions changed' }
        } finally { $image.Dispose(); $png.Dispose() }
    }
} finally { $reader.Dispose(); $stream.Dispose() }
# Missing and malformed inputs must replace stale game art with a usable fallback.
& $Converter -Source (Join-Path $Directory 'missing.png') -Output $output -Resource $resource
$fallback=[IO.File]::ReadAllBytes($output)
if ([Convert]::ToBase64String($fallback) -eq [Convert]::ToBase64String($original)) { throw 'Missing input retained stale artwork' }
[IO.File]::WriteAllText($source,'invalid png')
& $Converter -Source $source -Output $output -Resource $resource
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($output)) -ne [Convert]::ToBase64String($fallback)) { throw 'Invalid input did not use the same fallback' }
Write-Output 'ICO conversion: seven sizes, alpha, proportions, unchanged timestamps, missing/corrupt fallback passed.'
