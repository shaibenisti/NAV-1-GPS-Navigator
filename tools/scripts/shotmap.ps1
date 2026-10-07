<#
  shotmap.ps1 - the Map screenshots of docs\screenshots\map (the live part of shotdocs.ps1 for the map only):
  a replayed walk (/system/replay/demo.NMEA), the trace of a demo trip, zoom levels, heading up, the finished trip.
  Output: tools\scripts\shots\map_<name>.png (copy them to docs\screenshots\map). The demo trip is deleted again.
#>
$here = $PSScriptRoot
function Nav($c, $w = 2) { (& "$here\nav.ps1" $c -Wait $w *>&1 | Out-String) }
function Shot($name, $cmd = "", $settle = 2) {
  if ($cmd -ne "") { $cmd = "home;$cmd" }
  & "$here\shot.ps1" -Name "map_$name" -Open $cmd -Settle $settle | Out-Null
  Write-Host "  map_$name"
}
Nav "gps live" 1 | Out-Null; Nav "home" 1 | Out-Null
Nav "gps replay /system/replay/demo.NMEA 3" 3 | Out-Null
Start-Sleep 8
Nav "trip start" 2 | Out-Null
Start-Sleep 45
Nav "open Map" 4 | Out-Null
Nav "map zoom 3" 14 | Out-Null                                 # the zoom is only set while the app is open
Shot "following" "" 1
Nav "map zoom 6" 14 | Out-Null
Shot "street-level" "" 1
Nav "map zoom 3" 1 | Out-Null
Nav "map zoom 0" 16 | Out-Null
Shot "overview" "" 1
Nav "map zoom 3" 2 | Out-Null
$stop = Nav "trip stop" 6
Nav "gps live" 3 | Out-Null
Nav "map zoom 3" 2 | Out-Null
Nav "map up" 18 | Out-Null
Shot "heading-up" "" 1
Nav "map north" 2 | Out-Null
$base = [regex]::Match($stop, "saved (/data/trips/\S+?):").Groups[1].Value
Write-Host "trip: $base"
Nav "home" 1 | Out-Null
Nav "map trip $base" 16 | Out-Null
Shot "trip" "" 1
Nav "home" 1 | Out-Null
foreach ($e in "gpx", "csv", "json") { Nav "sd rm $base.$e" 2 | Out-Null }
Write-Host "done"
