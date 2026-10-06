<#
  mapload.ps1 - what the iPhone does when the offline map is browsed, from the PC, for N seconds:
  page + libraries once, then map pieces (HTTP range requests into /maps/israel.pmtiles) back to back.
  Screen refill timing during the load: console "refills" (restarted before, read after).

    .\tools\scripts\mapload.ps1 -Seconds 30
#>
param([int]$Seconds = 30, [string]$Ip = "192.168.4.1", [string]$Port = "COM3")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\NavSerial.ps1"

function Get-Refills {
  $o = (& "$PSScriptRoot\nav.ps1" "refills" -Wait 1 -Port $Port *>&1 | Out-String)
  if ($o -match "rst:0x") { Write-Host "  (device restarted when the port opened)" -ForegroundColor Yellow }
  if ($o -match "screen refills: .*") { return $Matches[0].Trim() }
  return "screen refills: no data"
}

[void](Get-Refills)                          # restart the counter
foreach ($f in "map", "map/leaflet.js", "map/pmleaflet.js", "map/pmtiles.js") { Invoke-WebRequest "http://$Ip/$f" -UseBasicParsing | Out-Null }
$cr = @((Invoke-WebRequest "http://$Ip/maps/israel.pmtiles" -Headers @{ Range = "bytes=0-0" } -UseBasicParsing).Headers["Content-Range"])[0]
$size = [int64]($cr -split "/")[-1]
$rnd = [Random]::new(1)
$n = 0; $bytes = 0; $t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  $from = [int64]($rnd.NextDouble() * ($size - 65536))
  $len = 4096 + $rnd.Next(60000)
  $r = Invoke-WebRequest "http://$Ip/maps/israel.pmtiles" -Headers @{ Range = "bytes=$from-$($from + $len - 1)" } -UseBasicParsing
  $n++; $bytes += $r.RawContentLength
}
$secs = ((Get-Date) - $t0).TotalSeconds
$b = Get-Refills
"map load: {0} requests, {1:N1} MB in {2:N0} s ({3:N0} KB/s)" -f $n, ($bytes / 1MB), $secs, ($bytes / 1KB / $secs)
"during the load: $b"
