# Generates app.ico for the Head Tracker Bridge (Windows PowerShell 5.1+, no
# external tools). Draws a 256x256 master -- headphones with a tracking dot on a
# dark rounded square -- then scales it to every standard icon size and packs a
# multi-image .ico with PNG-compressed entries.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
#
# The result is committed as app.ico, so this script only needs to run again
# when the artwork changes.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# ---- 256x256 master -------------------------------------------------------
$size = 256
$master = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($master)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

# Background: dark rounded square with a subtle vertical gradient (matches the
# GUI's kHeaderBg -> kWindowBg palette).
$bgPath = New-RoundedRectPath 8 8 240 240 52
$bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point(0, 0)), (New-Object System.Drawing.Point(0, $size)),
    [System.Drawing.Color]::FromArgb(255, 32, 38, 52), [System.Drawing.Color]::FromArgb(255, 16, 19, 27))
$g.FillPath($bgBrush, $bgPath)
$border = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 56, 64, 80), 4)
$g.DrawPath($border, $bgPath)

# Headband: top semicircle, accent blue, round caps.
$accent = [System.Drawing.Color]::FromArgb(255, 96, 165, 255)
$band = New-Object System.Drawing.Pen($accent, 26)
$band.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$band.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawArc($band, 58, 56, 140, 140, 180, 180)

# Ear cups: accent capsules hanging from the band ends.
$accentBrush = New-Object System.Drawing.SolidBrush($accent)
$leftCup = New-RoundedRectPath 42 118 38 66 17
$rightCup = New-RoundedRectPath 176 118 38 66 17
$g.FillPath($accentBrush, $leftCup)
$g.FillPath($accentBrush, $rightCup)

# Tracking dot: green, centred between the cups (the "live data" accent).
$okBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 94, 214, 140))
$g.FillEllipse($okBrush, 115, 158, 26, 26)

$g.Dispose()

# ---- scale + pack into app.ico -------------------------------------------
# Sizes up to 64 px are stored as classic uncompressed 32-bpp DIBs (readable by
# every icon consumer); 128/256 px are PNG-compressed as Windows recommends.
function ConvertTo-IconDib([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $bits = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($bits.Stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($bits.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($bits)
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40); $bw.Write([int]$s); $bw.Write([int]($s * 2))   # BITMAPINFOHEADER; height doubles for the AND mask
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($s * $s * 4)); $bw.Write([int]0); $bw.Write([int]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    for ($row = $s - 1; $row -ge 0; $row--) {                             # XOR image, bottom-up BGRA
        $bw.Write($pixels, $row * $bits.Stride, $s * 4)
    }
    $maskStride = [int]([math]::Ceiling($s / 32.0) * 4)                   # AND mask: all zero, alpha does the work
    $bw.Write((New-Object byte[] ($maskStride * $s)))
    $data = $ms.ToArray(); $bw.Dispose()
    return , $data
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$entries = @()
foreach ($s in $sizes) {
    $frame = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $fg = [System.Drawing.Graphics]::FromImage($frame)
    $fg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $fg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $fg.DrawImage($master, 0, 0, $s, $s)
    $fg.Dispose()
    if ($s -le 64) {
        $data = ConvertTo-IconDib $frame
    } else {
        $ms = New-Object System.IO.MemoryStream
        $frame.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $data = $ms.ToArray()
        $ms.Dispose()
    }
    $frame.Dispose()
    $entries += , @{ Size = $s; Data = $data }
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)                 # reserved
$bw.Write([uint16]1)                 # type: icon
$bw.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
    $bw.Write([byte]$dim); $bw.Write([byte]$dim)
    $bw.Write([byte]0); $bw.Write([byte]0)               # colors, reserved
    $bw.Write([uint16]1); $bw.Write([uint16]32)          # planes, bpp
    $bw.Write([uint32]$e.Data.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data) }

$target = Join-Path (Split-Path $PSScriptRoot -Parent) 'app.ico'
[System.IO.File]::WriteAllBytes($target, $out.ToArray())
$bw.Dispose()
Write-Host "Wrote $target ($($entries.Count) sizes: $($sizes -join ', '))"
