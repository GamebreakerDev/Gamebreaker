# Draws the launcher's icon (the project's own mark: a square split by a
# diagonal gap) and writes gamebreaker.ico with every size Windows asks for.
# Original artwork, drawn here; run again only if the mark changes.
#
# -Game draws the game's own icon instead (chosen 2026-11-16, "option A"): the
# same mark with the gap lit and the two pieces dimmed. Written to
# port\res\gamebreaker_game.ico by default.
#
# -Layout chooses how much of the dark tile shows around the mark:
#   inset  the mark in units 7 to 25 of the 32-unit tile (as first approved)
#   slim   the mark in units 3 to 29, a thin dark margin, corners following the tile's
#   bleed  the mark is the tile: dark shows only inside the gap
#
# -Gap chooses how wide the gap reads in each icon:
#   base    the pieces cut at x+y = 27.2 and 36.8 (32-unit grid, first layout): the
#           launcher's dark gap 9.6 wide, the game's light 7.2 wide with a dark
#           hairline each side, so the launcher's gap looks a third wider
#   narrow  the launcher's gap narrowed to the game's light (7.2); the game as base
#   wide    the game's light widened to the launcher's gap (9.6), its hairlines moved
#           outward by trimming the dimmed pieces; the launcher as base
#
# The defaults draw the project's icons: full bleed, the gap 7.2 wide in both.
# Run without -Layout or -Gap to redraw the
# committed files.
param([string]$Out = '', [string]$Preview, [switch]$Game, [ValidateSet('inset', 'slim', 'bleed')] [string]$Layout = 'bleed',
      [ValidateSet('base', 'narrow', 'wide')] [string]$Gap = 'narrow')
if (-not $Out) { $Out = $(if ($Game) { Join-Path $PSScriptRoot '..\..\port\res\gamebreaker_game.ico' } else { Join-Path $PSScriptRoot 'gamebreaker.ico' }) }

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# The mark's square in the 32-unit tile: origin, size, corner radius. All the
# mark's geometry below is relative to it (u, v from 0 to 1 across the square),
# so the three layouts are the same drawing at different sizes.
$geo = switch ($Layout) {
    'inset' { @{ o = 7.0; s = 18.0; r = 3.0 } }
    'slim'  { @{ o = 3.0; s = 26.0; r = 5.0 } }      # tile radius 7.5 less the 2.5 margin
    'bleed' { @{ o = 0.5; s = 31.0; r = 7.5 } }      # exactly the tile
}
# In the mark's own units: piece A's diagonal edge lies on u+v = 0.7333, piece
# B's on u+v = 1.2667 (the gap between them); the game's light fills u+v = 0.8
# to 1.2, leaving a thin dark line beside each piece.
$EA = 13.2 / 18; $EB = 22.8 / 18; $LA = 14.4 / 18; $LB = 21.6 / 18
if ($Gap -eq 'narrow' -and -not $Game) { $EA = $LA; $EB = $LB }                    # launcher gap = the game's light
if ($Gap -eq 'wide' -and $Game) { $LA = 13.2 / 18; $LB = 22.8 / 18; $EA = 12.0 / 18; $EB = 24.0 / 18 }   # game light = the launcher's gap

function New-MarkBitmap([int]$n) {
    $bmp = New-Object Drawing.Bitmap($n, $n, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)
    $k = $n / 32.0
    $o = $geo.o; $sz = $geo.s; $rr = $geo.r; $q = $sz / 18.0      # q: the mark's scale against the first layout
    function Pt([double]$u, [double]$v) { New-Object Drawing.PointF([single](($o + $u * $sz) * $k), [single](($o + $v * $sz) * $k)) }
    function Diag([double]$t) { ($o + $o + $t * $sz) * $k }          # u+v = t as x+y in pixels
    $dark = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 11, 12, 20))
    $square = New-RoundRect ($o * $k) ($o * $k) ($sz * $k) ($sz * $k) ($rr * $k)

    # The dark: the whole tile, or (full bleed) only the gap, so no dark rim
    # forms where the pieces' edge meets the tile's.
    if ($Layout -ne 'bleed') {
        $tile = New-RoundRect (0.5 * $k) (0.5 * $k) (31 * $k) (31 * $k) (7.5 * $k)
        $g.FillPath($dark, $tile)
    } else {
        $gapBand = New-Object Drawing.Drawing2D.GraphicsPath
        $gapBand.AddPolygon([Drawing.PointF[]]@(
            (New-Object Drawing.PointF((Diag ($EA - 0.02)), 0)), (New-Object Drawing.PointF((Diag ($EB + 0.02)), 0)),
            (New-Object Drawing.PointF(0, (Diag ($EB + 0.02)))), (New-Object Drawing.PointF(0, (Diag ($EA - 0.02))))))
        $gapReg = New-Object Drawing.Region($gapBand); $gapReg.Intersect($square)
        $g.FillRegion($dark, $gapReg)
    }

    # The two pieces.
    $from = Pt (-1 / 18) (-1 / 18); $to = Pt (19 / 18) (19 / 18)
    $d2 = 2 * $rr * $k
    $a = New-Object Drawing.Drawing2D.GraphicsPath      # upper-left piece
    $a.AddArc(($o * $k), ($o * $k), $d2, $d2, 180, 90)
    $a.AddLine((Pt ($rr / $sz) 0), (Pt $EA 0))
    $a.AddLine((Pt $EA 0), (Pt 0 $EA))
    $a.CloseFigure()
    $b = New-Object Drawing.Drawing2D.GraphicsPath      # lower-right piece
    $b.AddArc((($o + $sz - 2 * $rr) * $k), (($o + $sz - 2 * $rr) * $k), $d2, $d2, 0, 90)
    $b.AddLine((Pt (1 - $rr / $sz) 1), (Pt ($EB - 1) 1))
    $b.AddLine((Pt ($EB - 1) 1), (Pt 1 ($EB - 1)))
    $b.CloseFigure()
    if (-not $Game) {
        $brush = New-Object Drawing.Drawing2D.LinearGradientBrush($from, $to, [Drawing.Color]::FromArgb(255, 159, 144, 255), [Drawing.Color]::FromArgb(255, 18, 194, 176))
        $g.FillPath($brush, $a)
        $g.FillPath($brush, $b)
    } else {
        # Option A: the emphasis reversed, the gap lit "like the launcher's ring".
        # The pieces dimmed: the mark's gradient at low brightness.
        $dim = New-Object Drawing.Drawing2D.LinearGradientBrush($from, $to, [Drawing.Color]::FromArgb(255, 58, 54, 104), [Drawing.Color]::FromArgb(255, 20, 78, 82))
        $g.FillPath($dim, $a)
        $g.FillPath($dim, $b)
        $p1 = Pt (18.6 / 18) (-0.6 / 18); $p2 = Pt (-0.6 / 18) (18.6 / 18)
        $w0 = ($LB - $LA) * $sz / [Math]::Sqrt(2)          # the light's width across, in tile units
        # Light spilling onto the pieces' inner edges, falling off smoothly (many
        # thin layers, no visible bands). Clipped to the pieces themselves, so it
        # ends at their outline: it softens the dark lines' far side but never
        # covers them, and leaves no faint edge at the two lit corners.
        $pieces = New-Object Drawing.Region($a); $pieces.Union($b)
        $g.SetClip($pieces, [Drawing.Drawing2D.CombineMode]::Replace)
        for ($i = 28; $i -ge 1; $i--) {
            $pen = New-Object Drawing.Pen([Drawing.Color]::FromArgb(5, 160, 156, 255), [single](($w0 + (1.7 + $i * 0.16) * $q) * $k))
            $g.DrawLine($pen, $p1, $p2); $pen.Dispose()
        }
        $g.ResetClip()
        # The light, violet to teal, clipped to the mark's rounded square: its two
        # outer corners are rounded like the pieces', so the mark stays one
        # rounded square.
        $band = New-Object Drawing.Drawing2D.GraphicsPath
        $band.AddPolygon([Drawing.PointF[]]@(
            (New-Object Drawing.PointF((Diag $LA), 0)), (New-Object Drawing.PointF((Diag $LB), 0)),
            (New-Object Drawing.PointF(0, (Diag $LB))), (New-Object Drawing.PointF(0, (Diag $LA)))))
        $strip = New-Object Drawing.Region($band); $strip.Intersect($square)
        $lit = New-Object Drawing.Drawing2D.LinearGradientBrush((Pt 1 0), (Pt 0 1),
            [Drawing.Color]::FromArgb(255, 190, 178, 255), [Drawing.Color]::FromArgb(255, 90, 232, 214))
        $g.FillRegion($lit, $strip)
        if ($n -ge 48) {
            # A brighter core down the middle of the light, from 48 px up: many thin
            # layers, widest faintest, so it rises smoothly to the centre.
            $g.SetClip($strip, [Drawing.Drawing2D.CombineMode]::Replace)
            for ($i = 24; $i -ge 1; $i--) {
                $pen = New-Object Drawing.Pen([Drawing.Color]::FromArgb(9, 255, 255, 255), [single]((0.25 + $i * 0.15) * $q * $k))
                $g.DrawLine($pen, $p1, $p2); $pen.Dispose()
            }
            $g.ResetClip()
        }
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($n in $sizes) {
    $bmp = New-MarkBitmap $n
    $mem = New-Object IO.MemoryStream
    $bmp.Save($mem, [Drawing.Imaging.ImageFormat]::Png)
    if ($Preview -and $n -eq 256) { $bmp.Save($Preview, [Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    , $mem.ToArray()
}

# .ico container holding PNG images (valid since Windows Vista)
$file = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($file)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $n = $sizes[$i]
    $w.Write([byte]($n % 256)); $w.Write([byte]($n % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
[IO.File]::WriteAllBytes($Out, $file.ToArray())
Write-Host ("wrote {0} ({1} KB, sizes {2}, layout {3})" -f $Out, [Math]::Round($file.Length / 1KB), ($sizes -join ' '), $Layout)
