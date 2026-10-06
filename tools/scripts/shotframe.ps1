<#
  shotframe.ps1 - the README overview picture: a row of screenshots in a device-coloured frame (ImageMagick).
    .\tools\scripts\shotframe.ps1                # -> docs\images\os-overview.png
#>
param(
  [string[]]$Screens = @("home\page-1", "map\following", "trips\list", "gps\satellites", "compass\compass"),
  [string]$Out = "docs\images\os-overview.png",
  [int]$Height = 900
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$tmp = Join-Path $env:TEMP "navframes"
New-Item -ItemType Directory -Force $tmp | Out-Null
$frames = @()
$i = 0
foreach ($s in $Screens) {
  $src = Join-Path $root "docs\screenshots\$s.png"
  $dst = Join-Path $tmp ("f{0}.png" -f $i++)
  # 12 px frame in the case colour, rounded corners
  & magick $src -bordercolor "#26407f" -border 14 `
    "(" +clone -alpha transparent -fill white -draw "roundrectangle 0,0,507,827,40,40" ")" -compose DstIn -composite -compose over `
    -resize "x$Height" `
    "(" +clone -background "#000000" -shadow 40x14+0+12 ")" +swap -background none -layers merge +repage `
    -gravity center -background none -extent "$([int]($Height * 0.61) + 70)x$($Height + 60)" $dst
  $frames += $dst
}
$outFile = Join-Path $root $Out
New-Item -ItemType Directory -Force (Split-Path $outFile) | Out-Null
# soft light background; every frame already carries its shadow and some space around it
& magick $frames +repage +append -background "#eef0f6" -alpha remove -alpha off $outFile
Write-Host "written $outFile"
