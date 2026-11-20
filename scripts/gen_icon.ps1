param(
    [string]$OutPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'res\TapBPM.ico')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function New-RoundedRectPath {
    param(
        [float]$X,
        [float]$Y,
        [float]$W,
        [float]$H,
        [float]$Radius
    )
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $Radius * 2.0
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPngBytes {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        $s = [float]$Size
        $pad = [Math]::Max(1.0, $s * 0.05)
        $box = $s - 2.0 * $pad
        $rad = [Math]::Max(2.0, $s * 0.22)

        # Dark charcoal rounded square background (#121316) with subtle border (#2A303C)
        $bgPath = New-RoundedRectPath -X $pad -Y $pad -W $box -H $box -Radius $rad
        try {
            $bgBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 18, 19, 22))
            $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 42, 48, 60), [Math]::Max(1.0, $s * 0.04))
            try {
                $g.FillPath($bgBrush, $bgPath)
                $g.DrawPath($borderPen, $bgPath)
            } finally {
                $bgBrush.Dispose()
                $borderPen.Dispose()
            }
        } finally {
            $bgPath.Dispose()
        }

        $cx = $s * 0.5
        $cy = $s * 0.5

        # Outer concentric glow ring (#00E5A0 @ ~35% alpha)
        $outerR = $s * 0.34
        $outerPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(95, 0, 229, 160), [Math]::Max(1.0, $s * 0.055))
        try {
            $g.DrawEllipse($outerPen, $cx - $outerR, $cy - $outerR, $outerR * 2.0, $outerR * 2.0)
        } finally {
            $outerPen.Dispose()
        }

        # Crisp mid beat ring (#00E5A0)
        $midR = $s * 0.24
        $midPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(235, 0, 229, 160), [Math]::Max(1.2, $s * 0.065))
        try {
            $g.DrawEllipse($midPen, $cx - $midR, $cy - $midR, $midR * 2.0, $midR * 2.0)
        } finally {
            $midPen.Dispose()
        }

        # Subtle horizontal pulse ticks left & right
        $tickPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(200, 0, 229, 160), [Math]::Max(1.0, $s * 0.06))
        $tickPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $tickPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        try {
            $g.DrawLine($tickPen, $s * 0.14, $cy, $s * 0.25, $cy)
            $g.DrawLine($tickPen, $s * 0.75, $cy, $s * 0.86, $cy)
        } finally {
            $tickPen.Dispose()
        }

        # Central bright mint beat core dot
        $dotR = $s * 0.115
        $dotBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 229, 160))
        try {
            $g.FillEllipse($dotBrush, $cx - $dotR, $cy - $dotR, $dotR * 2.0, $dotR * 2.0)
        } finally {
            $dotBrush.Dispose()
        }

        $ms = New-Object System.IO.MemoryStream
        try {
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $bytes = [byte[]]$ms.ToArray()
            return ,$bytes
        } finally {
            $ms.Dispose()
        }
    } finally {
        $g.Dispose()
        $bmp.Dispose()
    }
}

$outDir = Split-Path -Parent $OutPath
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

$sizes = @(16, 32, 48, 256)
$frames = @()
foreach ($sz in $sizes) {
    $frames += [PSCustomObject]@{
        Size = $sz
        Bytes = New-IconPngBytes -Size $sz
    }
}

$fs = [System.IO.File]::Open($OutPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$bw = New-Object System.IO.BinaryWriter($fs)
try {
    # ICONDIR header
    $bw.Write([uint16]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]$frames.Count)

    $offset = 6 + (16 * $frames.Count)
    foreach ($frame in $frames) {
        $dim = if ($frame.Size -eq 256) { [byte]0 } else { [byte]$frame.Size }
        $bw.Write($dim)
        $bw.Write($dim)
        $bw.Write([byte]0)
        $bw.Write([byte]0)
        $bw.Write([uint16]1)
        $bw.Write([uint16]32)
        $bw.Write([uint32]$frame.Bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }

    foreach ($frame in $frames) {
        [byte[]]$data = $frame.Bytes
        $bw.Write($data, 0, $data.Length)
    }
} finally {
    $bw.Dispose()
    $fs.Dispose()
}

Write-Host "Generated $OutPath ($((Get-Item $OutPath).Length) bytes)"
