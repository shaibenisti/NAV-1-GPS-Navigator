<#
  maps.ps1 - build the NAV-1 offline maps on the PC and copy them to the SD card.

  Street map : OpenStreetMap data from a Protomaps daily build (vector PMTiles, ~185 MB for Israel, zoom 0-15).
  Satellite  : Sentinel-2 cloudless 2024 by EOX (10 m, zoom 0-14, ~330 MB), CC BY-NC-SA 4.0 = personal,
               non-commercial use with attribution. Fetched politely (4 requests at a time), resumable.
  Page       : tools\maps-www (index.html + Leaflet, protomaps-leaflet, pmtiles.js) -> <card>\www\map\
  NAV-1 serves /map (the page) and /maps/*.pmtiles (range requests) from the card.

  Examples:
    .\tools\scripts\maps.ps1 -Card G:                 # everything
    .\tools\scripts\maps.ps1 -Card G: -PageOnly       # only the page (after editing tools\maps-www)
#>
param(
  [string]$Card = "G:",
  [string]$Bbox = "34.2,29.45,35.95,33.35",    # west,south,east,north (Israel)
  [switch]$PageOnly,
  [switch]$NoSatellite
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$work = Join-Path $root "build\maps"
New-Item -ItemType Directory -Force $work, "$Card\www\map", "$Card\maps" | Out-Null

Copy-Item "$root\tools\maps-www\*" "$Card\www\map\" -Force
Write-Host "page copied to $Card\www\map"
if ($PageOnly) { return }

# pmtiles command-line tool (go-pmtiles)
$exe = Join-Path $work "pmtiles.exe"
if (-not (Test-Path $exe)) {
  $zip = Join-Path $work "pmtiles.zip"
  Invoke-WebRequest "https://github.com/protomaps/go-pmtiles/releases/download/v1.31.2/go-pmtiles_1.31.2_Windows_x86_64.zip" -OutFile $zip
  Expand-Archive $zip -DestinationPath $work -Force
}

# street map: newest Protomaps build
$builds = Invoke-RestMethod "https://build-metadata.protomaps.dev/builds.json"
$key = ($builds | Select-Object -Last 1).key
Write-Host "street map from build $key..."
& $exe extract "https://build.protomaps.com/$key" "$work\israel.pmtiles" --bbox=$Bbox --maxzoom=15
Copy-Item "$work\israel.pmtiles" "$Card\maps\israel.pmtiles" -Force

if (-not $NoSatellite) {
  Write-Host "satellite tiles (resumable)..."
  python "$PSScriptRoot\maps_sat_fetch.py" "$work\israel-sat.mbtiles"
  if (Test-Path "$work\israel-sat.pmtiles") { Remove-Item "$work\israel-sat.pmtiles" }
  & $exe convert "$work\israel-sat.mbtiles" "$work\israel-sat.pmtiles"
  Copy-Item "$work\israel-sat.pmtiles" "$Card\maps\israel-sat.pmtiles" -Force
}
Get-ChildItem "$Card\maps" | Format-Table Name, @{n = 'MB'; e = { [int]($_.Length / 1MB) } } -AutoSize
