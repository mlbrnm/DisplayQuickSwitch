# Original monitor-cat-in-a-cog artwork. Generates a multi-resolution, alpha-channel ICO
# using only Windows' System.Drawing; not required to build or run the app.
param([string]$OutputDirectory = "$PSScriptRoot\..\src\assets")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[void][System.IO.Directory]::CreateDirectory($OutputDirectory)
$accent = [System.Drawing.ColorTranslator]::FromHtml('#76B900')
$dark = [System.Drawing.ColorTranslator]::FromHtml('#101820')
$accentBrush = [System.Drawing.SolidBrush]::new($accent)
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
        $g.FillPolygon($accentBrush, $points.ToArray())
        $leftEar = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(113, 21), [System.Drawing.PointF]::new(225, 97), [System.Drawing.PointF]::new(126, 192))
        $rightEar = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(399, 21), [System.Drawing.PointF]::new(287, 97), [System.Drawing.PointF]::new(386, 192))
        $g.FillPolygon($accentBrush, $leftEar); $g.FillPolygon($accentBrush, $rightEar)
        $face = [System.Drawing.Drawing2D.GraphicsPath]::new()
        # Cat ears flow into a softly squared monitor bezel.
        $face.AddLine(108, 213, 143, 178)
        $face.AddLine(143, 178, 143, 100)
        $face.AddLine(143, 100, 223, 174)
        $face.AddBezier(223, 174, 243, 168, 269, 168, 289, 174)
        $face.AddLine(289, 174, 369, 100)
        $face.AddLine(369, 100, 369, 178)
        $face.AddLine(369, 178, 404, 213)
        $face.AddLine(404, 213, 404, 316)
        $face.AddBezier(404, 316, 404, 341, 391, 354, 366, 354)
        $face.AddLine(366, 354, 146, 354)
        $face.AddBezier(146, 354, 121, 354, 108, 341, 108, 316)
        $face.AddLine(108, 316, 108, 213)
        $face.CloseFigure(); $g.FillPath($darkBrush, $face)
        # A short neck and low, rounded foot suggest a desktop monitor.
        $g.FillRectangle($darkBrush, 237, 348, 38, 42)
        $stand = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $stand.AddLine(219, 380, 293, 380)
        $stand.AddBezier(293, 380, 304, 380, 312, 389, 312, 398)
        $stand.AddLine(312, 398, 200, 398)
        $stand.AddBezier(200, 398, 200, 389, 208, 380, 219, 380)
        $stand.CloseFigure(); $g.FillPath($darkBrush, $stand)
        $leftEye = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(163, 245), [System.Drawing.PointF]::new(232, 259), [System.Drawing.PointF]::new(206, 286), [System.Drawing.PointF]::new(175, 277))
        $rightEye = [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(349, 245), [System.Drawing.PointF]::new(280, 259), [System.Drawing.PointF]::new(306, 286), [System.Drawing.PointF]::new(337, 277))
        $g.FillPolygon($accentBrush, $leftEye); $g.FillPolygon($accentBrush, $rightEye)
        # A slider doubles as the cat's mouth; omit the finer second slider in tiny tray variants.
        $g.FillRectangle($accentBrush, 195, 309, 122, 12)
        $g.FillEllipse($accentBrush, 225, 296, 38, 38)
        if ($size -ge 32) {
            $g.FillRectangle($accentBrush, 214, 335, 84, 6)
            $g.FillEllipse($accentBrush, 270, 327, 22, 22)
        }
        $g.Dispose(); $face.Dispose(); $stand.Dispose()
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
} finally { $accentBrush.Dispose(); $darkBrush.Dispose() }
