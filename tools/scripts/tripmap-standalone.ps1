<#
  tripmap-standalone.ps1 - one self-contained HTML page for a NAV-1 trip: the track coloured by
  speed over OpenStreetMap and satellite tiles that are downloaded for the route's area and embedded
  (the page loads no tiles itself, so it works as a Claude artifact or offline).

  .\tools\scripts\tripmap-standalone.ps1 trip.gpx -Json trip.json -Title "Dog walk" -Out page.html
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory, Position = 0)][string]$Gpx,
  [string]$Json = "",
  [string]$Title = "",
  [string]$Note = "",
  [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture

$xml = [xml](Get-Content $Gpx -Raw)
$ns = New-Object Xml.XmlNamespaceManager $xml.NameTable
$ns.AddNamespace("g", $xml.DocumentElement.NamespaceURI)
$nodes = @($xml.SelectNodes("//g:trkpt", $ns))
$t0 = [datetime]::Parse($nodes[0].time, $inv).ToUniversalTime()
$pts = $nodes | ForEach-Object {
  "[{0},{1},{2}]" -f $_.lat, $_.lon, [int]([datetime]::Parse($_.time, $inv).ToUniversalTime() - $t0).TotalSeconds }
$lat = $nodes | ForEach-Object { [double]$_.lat }; $lon = $nodes | ForEach-Object { [double]$_.lon }
$la0 = ($lat | Measure-Object -Minimum).Minimum; $la1 = ($lat | Measure-Object -Maximum).Maximum
$lo0 = ($lon | Measure-Object -Minimum).Minimum; $lo1 = ($lon | Measure-Object -Maximum).Maximum
$pl = [math]::Max(0.002, ($la1 - $la0)); $po = [math]::Max(0.002, ($lo1 - $lo0))   # margin around the route

function TileXY([double]$la, [double]$lo, [int]$z) {
  $n = [math]::Pow(2, $z)
  $x = [math]::Floor(($lo + 180) / 360 * $n)
  $r = $la * [math]::PI / 180
  $y = [math]::Floor((1 - [math]::Log([math]::Tan($r) + 1 / [math]::Cos($r)) / [math]::PI) / 2 * $n)
  return @([int]$x, [int]$y)
}
$sources = @{
  osm = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
  sat = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"
}
$tiles = [ordered]@{}
$bytes = 0
foreach ($z in 15..18) {
  $a = TileXY ($la1 + $pl) ($lo0 - $po) $z; $b = TileXY ($la0 - $pl) ($lo1 + $po) $z
  foreach ($x in $a[0]..$b[0]) { foreach ($y in $a[1]..$b[1]) { foreach ($k in $sources.Keys) {
    $url = $sources[$k].Replace("{z}", "$z").Replace("{x}", "$x").Replace("{y}", "$y")
    $r = Invoke-WebRequest $url -UserAgent "NAV-1 trip map (personal, one route)" -TimeoutSec 30
    $mime = if ($r.Headers["Content-Type"]) { "$($r.Headers['Content-Type'])" } else { "image/png" }
    $tiles["$k/$z/$x/$y"] = "data:$mime;base64," + [Convert]::ToBase64String($r.Content)
    $bytes += $r.Content.Length
    Start-Sleep -Milliseconds 120                 # be polite to the tile servers
  } } }
}
Write-Host ("{0} tiles, {1:N0} KB" -f $tiles.Count, ($bytes / 1KB))

$leafletCss = (Invoke-WebRequest "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css").Content
if ($leafletCss -is [byte[]]) { $leafletCss = [Text.Encoding]::UTF8.GetString($leafletCss) }

$s = if ($Json) { Get-Content $Json -Raw | ConvertFrom-Json } else { $null }
function Clock([int]$sec) { "{0}:{1:00}" -f [int][math]::Floor($sec / 60), ($sec % 60) }
$stats = if ($s) {
  @(("{0:0.00} km" -f $s.distance_km), "Distance"), @((Clock $s.duration_s), "Duration"), @((Clock $s.moving_s), "Moving"),
  @(("{0:0.0} km/h" -f $s.avg_kmh), "Avg moving"), @(("{0:0.0} km/h" -f $s.max_kmh), "Max"), @("$($s.points)", "Track points") |
    ForEach-Object { "<div class=""stat""><b>$($_[0])</b><span>$($_[1])</span></div>" }
} else { @("<div class=""stat""><b>$($nodes.Count)</b><span>Track points</span></div>") }
$when = $t0.ToLocalTime().ToString("dddd d MMMM yyyy, HH:mm", $inv)
if (-not $Title) { $Title = "Trip " + $t0.ToLocalTime().ToString("d MMM", $inv) }

$page = (Get-Content (Join-Path $PSScriptRoot "tripmap-page.html") -Raw).
  Replace("__TITLE__", $Title).Replace("__HEADING__", $Title).
  Replace("__SUBTITLE__", "$when &middot; recorded by NAV-1 ($($xml.gpx.creator))").
  Replace("__STATS__", ($stats -join "")).Replace("__NOTE__", $Note).
  Replace("__LEAFLET_CSS__", $leafletCss).
  Replace("__POINTS__", "[" + ($pts -join ",") + "]").
  Replace("__TILES__", ($tiles | ConvertTo-Json -Compress))
[IO.File]::WriteAllText($Out, $page)
Write-Host ("{0} ({1:N0} KB)" -f $Out, ((Get-Item $Out).Length / 1KB))
