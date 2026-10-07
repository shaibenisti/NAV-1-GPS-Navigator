<#
  tripmap.ps1 - show a NAV-1 trip on a map (local HTML page, OpenStreetMap, opens in the browser).

  Examples:
    .\tools\scripts\tripmap.ps1 walk.gpx                 # a GPX file on the PC
    .\tools\scripts\tripmap.ps1                          # download the newest trip from NAV-1 (USB console)
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Position = 0)][string]$Gpx = "",
  [string]$Port = "COM3",
  [switch]$NoOpen
)
. "$PSScriptRoot\NavSerial.ps1"
$outDir = Join-Path $PSScriptRoot "output\maps"
New-Item -ItemType Directory -Force $outDir | Out-Null

if (-not $Gpx) {                                   # newest trip from the device
  Open-Nav $Port
  try {
    $l = Invoke-Nav "trip list" 3
    $m = [regex]::Match($l, "\[TRIP\] (\S+)")
    if (-not $m.Success) { throw "no trips on NAV-1" }
    $name = $m.Groups[1].Value
    Send-Nav "sd cat /data/trips/$($name.Substring(0, 4))/$name.gpx"
    $r = Wait-NavFor "<<<END [^>]*>>>" 120
    $x = [regex]::Match($r, "(?s)<<<BEGIN \S+ \d+>>>\r?\n(.*)<<<END")
    if (-not $x.Success) { throw "GPX download failed" }
    $Gpx = Join-Path $outDir "$name.gpx"
    [IO.File]::WriteAllText($Gpx, $x.Groups[1].Value)
  } finally { Close-Nav }
}

$xml = [xml](Get-Content $Gpx -Raw)
$ns = New-Object Xml.XmlNamespaceManager $xml.NameTable
$ns.AddNamespace("g", $xml.DocumentElement.NamespaceURI)
$inv = [Globalization.CultureInfo]::InvariantCulture
$pts = @($xml.SelectNodes("//g:trkpt", $ns) | ForEach-Object {
  $spd = $_.SelectSingleNode("g:extensions//*[local-name()='speed']", $ns)
  "[{0},{1},{2}]" -f $_.lat, $_.lon, $(if ($spd) { ([double]$spd.InnerText * 3.6).ToString("0.0", $inv) } else { "null" })
})
if ($pts.Count -lt 2) { throw "no track points in $Gpx" }
$title = [IO.Path]::GetFileNameWithoutExtension($Gpx)

$html = @"
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>NAV-1 trip $title</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/leaflet@1.9.4/dist/leaflet.css">
<script src="https://cdn.jsdelivr.net/npm/leaflet@1.9.4/dist/leaflet.js"></script>
<style>html,body,#map{height:100%;margin:0}#info{position:absolute;z-index:1000;top:10px;right:10px;background:#fff;
padding:8px 12px;border-radius:8px;font:14px system-ui;box-shadow:0 1px 4px #0004}</style></head><body>
<div id="map"></div><div id="info"></div><script>
const pts = [$($pts -join ",")];
const map = L.map('map');
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom: 19, attribution: '&copy; OpenStreetMap'}).addTo(map);
const line = L.polyline(pts.map(p => [p[0], p[1]]), {color: '#1E88E5', weight: 5}).addTo(map);
L.circleMarker(pts[0], {radius: 8, color: '#43A047', fillOpacity: 1}).addTo(map).bindTooltip('Start');
L.circleMarker(pts[pts.length - 1], {radius: 8, color: '#E53935', fillOpacity: 1}).addTo(map).bindTooltip('End');
map.fitBounds(line.getBounds(), {padding: [30, 30]});
let m = 0; for (let i = 1; i < pts.length; i++) m += map.distance(pts[i - 1], pts[i]);
document.getElementById('info').innerHTML = '<b>$title</b><br>' + pts.length + ' points, ' + (m / 1000).toFixed(2) + ' km';
</script></body></html>
"@
$file = Join-Path $outDir "$title.html"
[IO.File]::WriteAllText($file, $html)
Write-Host "$file ($($pts.Count) points)"
if (-not $NoOpen) { Start-Process $file }
