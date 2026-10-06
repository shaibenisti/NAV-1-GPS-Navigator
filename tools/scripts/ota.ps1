<#
  ota.ps1 - build NAV-1 and install it over Wi-Fi (no USB flashing), then follow the reboot
  and the new firmware's self-verification (rollback if it fails).

  The device only accepts an upload while ARMED: this script arms it over the console
  (USB cable = physical access). Without the cable, arm it on the device (Tools) and use -NoArm.

  Examples:
    .\tools\scripts\ota.ps1                    # build + arm + upload + verify
    .\tools\scripts\ota.ps1 -NoBuild           # send the last build (build\ota)
    .\tools\scripts\ota.ps1 -NoArm -Ip 192.168.1.50   # device address on your Wi-Fi (192.168.4.1 on the NAV-1 hotspot)

  Builds with idf-build.ps1 (ESP-IDF).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [string]$Port = "COM3",
  [string]$Ip = "",
  [switch]$NoBuild,
  [switch]$NoArm,
  [string]$Bin = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\NavSerial.ps1"
$root = Resolve-Path "$PSScriptRoot\..\.."
$out = Join-Path $root "build\ota"

if (-not $Bin) {
  if (-not $NoBuild) {
    Write-Host "[1/4] build..." -ForegroundColor Cyan
    $script = Join-Path $root "idf-build.ps1"
    $log = & $script -OutDir $out *>&1 | Out-String
    if ($log -notmatch "firmware \d+ bytes") { Write-Host $log; throw "build failed" }
  }
  $Bin = Join-Path $out "NAV1.bin"
}
if (-not (Test-Path $Bin)) { throw "no firmware image at $Bin" }
$expect = if (Test-Path "$root\firmware\build_info.h") {
  ([regex]::Match((Get-Content "$root\firmware\build_info.h" -Raw), 'FW_GIT_DESCRIBE "([^"]+)"')).Groups[1].Value } else { "" }
Write-Host ("image {0} ({1:N0} KB), build {2}" -f $Bin, ((Get-Item $Bin).Length / 1KB), $expect)

if (-not $NoArm -or -not $Ip) {
  Open-Nav $Port
  try {
    if (-not $Ip) {
      $w = Invoke-Nav "diag wifi" 1
      if ($w -notmatch "connected '.*?' ip ([\d.]+)") { throw "NAV-1 is not on Wi-Fi: $w" }
      $Ip = $Matches[1]
    }
    if (-not $NoArm) {
      Write-Host "[2/4] arming over the console..." -ForegroundColor Cyan
      if ((Invoke-Nav "ota arm" 1) -notmatch "\[OTA\] armed") { throw "could not arm" }
    }
  } finally { Close-Nav }
}
$before = Invoke-RestMethod "http://$Ip/api/status" -TimeoutSec 8
Write-Host "device $($before.name) at $Ip runs $($before.build) from $($before.slot)"

Write-Host "[3/4] uploading..." -ForegroundColor Cyan
$sw = [Diagnostics.Stopwatch]::StartNew()
try {
  $r = Invoke-RestMethod "http://$Ip/api/update" -Method Post -Form @{ file = Get-Item $Bin } -TimeoutSec 300
} catch {
  $msg = $_.ErrorDetails.Message
  throw "upload refused: $(if ($msg) { $msg } else { $_.Exception.Message })"
}
if (-not $r.ok) { throw "upload failed: $($r.error)" }
$up = $sw.Elapsed.TotalSeconds
Write-Host ("  sent + verified in {0:N1} s ({1:N0} KB/s): {2}" -f $up, ((Get-Item $Bin).Length / 1KB / $up), $r.message)

Write-Host "[4/4] waiting for the reboot and the self-verification (~25 s)..." -ForegroundColor Cyan
$end = (Get-Date).AddSeconds(90)
$seen = $null
while ((Get-Date) -lt $end) {
  Start-Sleep -Seconds 2
  try { $s = Invoke-RestMethod "http://$Ip/api/status" -TimeoutSec 3 } catch { continue }
  if ($s.slot -eq $before.slot -and $s.uptime_s -gt $before.uptime_s) { continue }   # not rebooted yet
  if (-not $seen) { $seen = $s; Write-Host ("  back up after {0:N0} s: {1} from {2}, {3}" -f $sw.Elapsed.TotalSeconds, $s.build, $s.slot, $s.ota) }
  if ($s.ota -ne "verifying") { break }
}
if (-not $seen) { throw "the device did not come back on Wi-Fi within 90 s" }
if ($s.slot -eq $before.slot) { throw "rolled back: still running $($s.build) from $($s.slot)" }
if ($s.ota -eq "verifying") { throw "new firmware still not verified after 90 s" }
if ($expect -and $s.build -ne $expect) { Write-Host "  note: running $($s.build), expected $expect" -ForegroundColor Yellow }
Write-Host ("OK: {0} verified and kept, slot {1} ({2:N0} s total)" -f $s.build, $s.slot, $sw.Elapsed.TotalSeconds) -ForegroundColor Green
