<#
  shotdocs.ps1 - screenshots of every screen reachable from the console, for the documentation
  (docs\screenshots\<app>\<name>.png, full 480 x 800 PNG, lossless).

  The live screens (GPS, Compass, Map, Trips) need a position. They use a recorded walk that was moved to a
  neutral place with shiftnmea.py and put on the card as /system/replay/demo.NMEA (it starts with a fix):
      python tools\scripts\shiftnmea.py <raw capture> demo.NMEA <lat> <lon> [altitude offset]
      .\tools\scripts\sdput.ps1 demo.NMEA /system/replay/demo.NMEA
  The demo trip recorded during the run is deleted from the card again at the end.
#>
param([string]$Replay = "/system/replay/demo.NMEA")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out = Join-Path (Resolve-Path "$here\..\..").Path "docs\screenshots"

function Nav($c, $w = 2) { (& "$here\nav.ps1" $c -Wait $w *>&1 | Out-String) }
function Shot($group, $name, $cmd = "", $settle = 2) {
  New-Item -ItemType Directory -Force "$out\$group" | Out-Null
  if ($cmd -ne "") { $cmd = "home;$cmd" }                      # the console opens apps from Home only
  & "$here\shot.ps1" -Name "docs_tmp" -Open $cmd -Settle $settle | Out-Null
  Move-Item -Force "$here\shots\docs_tmp.png" "$out\$group\$name.png"
  Write-Host "  $group/$name"
}

Write-Host "static screens"
Nav "gps live" 1 | Out-Null
Nav "wifi off" 3 | Out-Null                                  # no network names or addresses on the screens
Nav "ble off" 2 | Out-Null
Shot "home" "page-1" "home"
Shot "home" "page-2" "swipe"
Nav "swipe" 1 | Out-Null
Nav "wifi hotspot on" 8 | Out-Null
Shot "phone" "phone" "open Phone" 4
# hide the QR code and the hotspot name / password in the text under it
& magick "$out\phone\phone.png" "(" +clone -crop 300x300+90+132 -scale 6% -scale 1667% ")" -geometry +90+132 -composite "$out\phone\phone.png"
& magick "$out\phone\phone.png" "(" +clone -crop 450x75+15+520 -scale 4% -scale 2500% ")" -geometry +15+520 -composite "$out\phone\phone.png"
Nav "wifi hotspot off" 3 | Out-Null
Shot "files" "root" "files /;open Files"
Shot "storage" "storage" "open Storage"
$n = @("wifi", "bluetooth", "hotspot", "display", "date-time", "about")
foreach ($p in 0..5) { Shot "settings" $n[$p] "page Settings $p;open Settings" }
$n = @("health", "touch-test", "update")
foreach ($p in 0..2) { Shot "tools" $n[$p] "page Tools $p;open Tools" }

Write-Host "live screens (replay of $Replay at 3x)"
Nav "home" 1 | Out-Null
Nav "gps replay $Replay 3" 3 | Out-Null
Start-Sleep 8
Nav "trip start" 2 | Out-Null
Start-Sleep 45                                                  # the trace builds up
Shot "map" "following" "open Map" 12
Nav "map zoom 6" 12 | Out-Null
Shot "map" "street-level" "" 1
Nav "map zoom 3" 1 | Out-Null
Nav "map up" 12 | Out-Null
Shot "map" "heading-up" "" 1
Nav "map north" 1 | Out-Null
Nav "map zoom 0" 14 | Out-Null
Shot "map" "overview" "" 1
Nav "map zoom 3" 1 | Out-Null
Shot "gps" "position" "page GPS 0;open GPS"
Shot "gps" "satellites" "page GPS 1;open GPS"
Shot "gps" "details" "page GPS 2;open GPS"
Shot "compass" "compass" "open Compass" 4
Shot "trips" "record" "page Trips 0;open Trips" 3
$stop = Nav "trip stop" 6
Nav "gps live" 2 | Out-Null
$base = [regex]::Match($stop, "saved (/data/trips/\S+?):").Groups[1].Value
Write-Host "demo trip: $base"
Shot "trips" "list" "page Trips 1;open Trips" 3
Shot "trips" "totals" "page Trips 2;open Trips" 3
if ($base) {
  Nav "home" 1 | Out-Null
  Nav "map trip $base" 12 | Out-Null
  Shot "map" "trip" "" 1
  Nav "home" 1 | Out-Null
  foreach ($e in "gpx", "csv", "json") { Nav "sd rm $base.$e" 2 | Out-Null }
}
Nav "home" 1 | Out-Null
Nav "wifi on" 2 | Out-Null                                   # the setting the device had before
Write-Host "done: $out"
