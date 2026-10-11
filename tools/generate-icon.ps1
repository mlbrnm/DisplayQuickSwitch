# Original cat-in-a-cog artwork. Generates a multi-resolution, alpha-channel ICO
# using only Windows' System.Drawing; not required to build or run the app.
param([string]$OutputDirectory = "$PSScriptRoot\..\src\assets")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[void][System.IO.Directory]::CreateDirectory($OutputDirectory)
$green = [System.Drawing.ColorTranslator]::FromHtml('#76B900')
$dark = [System.Drawing.ColorTranslator]::FromHtml('#101820')
$greenBrush = [System.Drawing.SolidBrush]::new($green)
$darkBrush = [System.Drawing.SolidBrush]::new($dark)
$images = @()
try {
    foreach ($size in @(16, 20, 24, 32, 40, 48, 64, 128, 256)) {
        # Render supersampled then downsample, keeping tray edges clean at small sizes.
        $large = [System.Drawing.Bitmap]::new(1024, 1024, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($large)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.ScaleTransform(2, 2)
        $points = [System.Collections.Generic.List[System.Drawing.PointF]]::new()
        for ($tooth = 0; $tooth -lt 8; $tooth++) {
            foreach ($corner in @(@(-22.5, 194), @(-12, 228), @(12, 228), @(22.5, 194))) {
                $angle = ($tooth * 45 + $corner[0]) * [math]::PI / 180
                $points.Add([System.Drawing.PointF]::new(256 + $corner[1] * [math]::Cos($angle), 256 + $corner[1] * [math]::Sin($angle)))
            }
        }
        $g.FillPolygon($greenBrush, $points.ToArray())
        $leftEar = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(113, 21), [System.Drawing.PointF]::new(225, 97), [System.Drawing.PointF]::new(126, 192))
        $rightEar = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(399, 21), [System.Drawing.PointF]::new(287, 97), [System.Drawing.PointF]::new(386, 192))
        $g.FillPolygon($greenBrush, $leftEar); $g.FillPolygon($greenBrush, $rightEar)
        $face = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $face.AddLine(145, 221, 143, 100)
        $face.AddLine(143, 100, 223, 174)
        $face.AddBezier(223, 174, 243, 168, 269, 168, 289, 174)
        $face.AddLine(289, 174, 369, 100)
        $face.AddLine(369, 100, 367, 221)
        $face.AddBezier(367, 221, 426, 305, 365, 390, 256, 394)
        $face.AddBezier(256, 394, 147, 390, 86, 305, 145, 221)
        $face.CloseFigure(); $g.FillPath($darkBrush, $face)
        $leftEye = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(163, 245), [System.Drawing.PointF]::new(232, 259), [System.Drawing.PointF]::new(206, 286), [System.Drawing.PointF]::new(175, 277))
        $rightEye = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(349, 245), [System.Drawing.PointF]::new(280, 259), [System.Drawing.PointF]::new(306, 286), [System.Drawing.PointF]::new(337, 277))
        $g.FillPolygon($greenBrush, $leftEye); $g.FillPolygon($greenBrush, $rightEye)
        # A slider doubles as the cat's mouth; omit the finer second slider in tiny tray variants.
        $g.FillRectangle($greenBrush, 195, 317, 122, 12)
        $g.FillEllipse($greenBrush, 225, 304, 38, 38)
        if ($size -ge 32) {
            $g.FillRectangle($greenBrush, 214, 351, 84, 8)
            $g.FillEllipse($greenBrush, 270, 341, 28, 28)
        }
        $g.Dispose(); $face.Dispose()
        $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $scaled = [System.Drawing.Graphics]::FromImage($bitmap)
        $scaled.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $scaled.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $scaled.DrawImage($large, 0, 0, $size, $size)
        $scaled.Dispose(); $large.Dispose()
        if ($size -eq 256) { $bitmap.Save((Join-Path $OutputDirectory 'quickswitch.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
        $stream = [System.IO.MemoryStream]::new()
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $images += @{ Size = $size; Bytes = $stream.ToArray() }
        $stream.Dispose(); $bitmap.Dispose()
    }
    $file = [System.IO.File]::Create((Join-Path $OutputDirectory 'quickswitch.ico'))
    $writer = [System.IO.BinaryWriter]::new($file)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$images.Count)
        $offset = 6 + 16 * $images.Count
        foreach ($image in $images) {
            $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$image.Bytes.Length); $writer.Write([uint32]$offset)
            $offset += $image.Bytes.Length
        }
        foreach ($image in $images) { $writer.Write([byte[]]$image.Bytes) }
    } finally { $writer.Dispose() }
} finally { $greenBrush.Dispose(); $darkBrush.Dispose() }
