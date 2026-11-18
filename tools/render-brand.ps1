[CmdletBinding()]
param(
    # Also write the mark alone at 512 px on dark and light backgrounds, as
    # <PreviewPath>-dark.png and <PreviewPath>-light.png.
    [string]$PreviewPath = ''
)

# Renders the README banner (.github/shiftglue-banner.png) from the vector
# mark in .github/shiftglue-mark.svg, so the art has one source. The mark is
# the union of the paths in <g id="silhouette"> minus the path in
# <g id="cutout"> (the gear knob, lever and road the Shift marks share), in a
# 400 x 420 box. The banner copies Lavender Shift's layout and font.
# Runs under Windows PowerShell (WPF needs an STA thread and Bahnschrift).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase

$root = Split-Path -Parent $PSScriptRoot
[xml]$svg = Get-Content -Raw -LiteralPath (Join-Path $root '.github/shiftglue-mark.svg')
$ns = New-Object Xml.XmlNamespaceManager $svg.NameTable
$ns.AddNamespace('s', 'http://www.w3.org/2000/svg')
$silhouette = $null
foreach ($path in $svg.SelectNodes("//s:g[@id='silhouette']//s:path", $ns)) {
    $part = [Windows.Media.Geometry]::Parse($path.GetAttribute('d'))
    $silhouette = if ($silhouette) {
        New-Object Windows.Media.CombinedGeometry ([Windows.Media.GeometryCombineMode]::Union), $silhouette, $part
    } else { $part }
}
$cutout = [Windows.Media.Geometry]::Parse($svg.SelectSingleNode("//s:g[@id='cutout']//s:path", $ns).GetAttribute('d'))
$geometry = New-Object Windows.Media.CombinedGeometry ([Windows.Media.GeometryCombineMode]::Exclude), $silhouette, $cutout

# Palette: the mark's colour on dark and on light, and the banner's card.
$markDark = '#F2C45D'
$markLight = '#805600'
$card = '#19150D'
$titleColor = '#F6F1E7'
$taglineColor = '#B3A68C'

$brushOf = { param($c) New-Object Windows.Media.SolidColorBrush ([Windows.Media.ColorConverter]::ConvertFromString($c)) }

function Save-Visual {
    param($Visual, [int]$Width, [int]$Height, [string]$Path)
    $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap $Width, $Height, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($Visual)
    $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path))
    $stream = [IO.File]::Create($Path)
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
}

# The README banner: the mark and the name, at 2x for high-DPI screens.
$width = 1600; $height = 440
$visual = New-Object Windows.Media.DrawingVisual
$context = $visual.RenderOpen()
$context.DrawRoundedRectangle((& $brushOf $card), $null, (New-Object Windows.Rect 0, 0, $width, $height), 36, 36)
$scale = 300 / 420
$context.PushTransform((New-Object Windows.Media.TranslateTransform 150, 70))
$context.PushTransform((New-Object Windows.Media.ScaleTransform $scale, $scale))
$context.DrawGeometry((& $brushOf $markDark), $null, $geometry)
$context.Pop(); $context.Pop()
$culture = [Globalization.CultureInfo]::InvariantCulture
$title = New-Object Windows.Media.FormattedText 'ShiftGlue', $culture, ([Windows.FlowDirection]::LeftToRight),
    (New-Object Windows.Media.Typeface 'Bahnschrift SemiBold'), 128, (& $brushOf $titleColor), 1.0
$tagline = New-Object Windows.Media.FormattedText 'The Xbox 360 recompiler behind the Shift ports', $culture,
    ([Windows.FlowDirection]::LeftToRight), (New-Object Windows.Media.Typeface 'Bahnschrift'), 44, (& $brushOf $taglineColor), 1.0
$context.DrawText($title, (New-Object Windows.Point 470, 120))
$context.DrawText($tagline, (New-Object Windows.Point 476, 272))
$context.Close()
Save-Visual $visual $width $height (Join-Path $root '.github/shiftglue-banner.png')

if ($PreviewPath) {
    foreach ($variant in @(@('dark', $markDark, '#0E0B07'), @('light', $markLight, '#FFFFFF'))) {
        $visual = New-Object Windows.Media.DrawingVisual
        $context = $visual.RenderOpen()
        $context.DrawRectangle((& $brushOf $variant[2]), $null, (New-Object Windows.Rect 0, 0, 512, 512))
        $scale = (512 * 0.94) / 420
        $context.PushTransform((New-Object Windows.Media.TranslateTransform ((512 - 400 * $scale) / 2), (512 * 0.03)))
        $context.PushTransform((New-Object Windows.Media.ScaleTransform $scale, $scale))
        $context.DrawGeometry((& $brushOf $variant[1]), $null, $geometry)
        $context.Pop(); $context.Pop(); $context.Close()
        Save-Visual $visual 512 512 "$PreviewPath-$($variant[0]).png"
    }
}
'ok'
