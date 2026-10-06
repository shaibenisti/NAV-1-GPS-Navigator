<#
  fieldtest.ps1 - fetch and analyse NAV-1 outdoor field tests (Tools > Field test).

    .\tools\scripts\fieldtest.ps1                 # download + analyse the latest test from the device
    .\tools\scripts\fieldtest.ps1 -List           # list the tests on the device
    .\tools\scripts\fieldtest.ps1 -Id FT-20260928-101500
    .\tools\scripts\fieldtest.ps1 -Stop           # stop a running test first, then download it
    .\tools\scripts\fieldtest.ps1 -From F:\FIELD\FT-20260928-101500   # SD card in a card reader (fast)
    .\tools\scripts\fieldtest.ps1 -Local FT-20260928-101500            # re-analyse an already downloaded test

  Data lands in tools\scripts\fieldtests\<id>\ (samples.csv, events.csv, summary.json from the
  device, plus report.txt and analysis.json from this script). Serial download runs at ~10 KB/s
  (a 1-hour test is ~0.9 MB, ~1.5 min); for long tests the card reader is faster.
  Format of the files: firmware/src/diag/FieldRecorder.h.
#>
param(
  [string]$Port = "COM3",
  [string]$Id = "",
  [switch]$List,
  [switch]$Stop,
  [string]$From = "",
  [string]$Local = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\NavSerial.ps1"
$store = Join-Path $PSScriptRoot "fieldtests"

# ---------------------------------------------------------------------------------------------
function Get-DeviceFile([string]$Path) {
  [void](Receive-Nav)
  $start = (Get-NavLog).Length
  Send-Nav "sd cat $Path"
  $deadline = (Get-Date).AddSeconds(10)
  $text = ""
  while ($true) {                                         # look at everything received since the request
    [void](Receive-Nav)
    $text = (Get-NavLog).Substring($start)
    if ($text -match "cannot open") { return $null }
    if ($text -match "<<<END [^>]*>>>") { break }
    if ($text -match "<<<BEGIN [^\r\n]* (\d+)>>>" -and $deadline -lt (Get-Date).AddSeconds(5)) {
      $deadline = (Get-Date).AddSeconds([math]::Max(15, [int]$Matches[1] / 8000 + 10))   # ~10 KB/s
    }
    if ((Get-Date) -gt $deadline) { throw "download of $Path timed out" }
    Start-Sleep -Milliseconds 50
  }
  $b = $text.IndexOf(">>>", $text.IndexOf("<<<BEGIN")) + 3
  if ($text[$b] -eq "`r") { $b++ }
  if ($text[$b] -eq "`n") { $b++ }
  $e = $text.LastIndexOf("<<<END")
  return $text.Substring($b, $e - $b).TrimEnd("`r", "`n") + "`n"
}

function Get-Haversine([double]$la1, [double]$lo1, [double]$la2, [double]$lo2) {
  $k = [math]::PI / 180; $dLa = ($la2 - $la1) * $k; $dLo = ($lo2 - $lo1) * $k
  $a = [math]::Sin($dLa / 2) * [math]::Sin($dLa / 2) + [math]::Cos($la1 * $k) * [math]::Cos($la2 * $k) * [math]::Sin($dLo / 2) * [math]::Sin($dLo / 2)
  return 2 * 6371000 * [math]::Atan2([math]::Sqrt($a), [math]::Sqrt(1 - $a))
}

function Invoke-Analysis([string]$Dir) {
  $id = Split-Path $Dir -Leaf
  $samples = @(Import-Csv (Join-Path $Dir "samples.csv"))
  $events = if (Test-Path (Join-Path $Dir "events.csv")) { @(Import-Csv (Join-Path $Dir "events.csv")) } else { @() }
  $device = if (Test-Path (Join-Path $Dir "summary.json")) { Get-Content (Join-Path $Dir "summary.json") -Raw | ConvertFrom-Json } else { $null }
  if (-not $samples.Count) { throw "no samples in $Dir" }
  $num = { param($v) if ($v -ne $null -and "$v" -ne "") { [double]$v } else { $null } }

  $first = $samples[0]; $last = $samples[-1]
  $fixRows = @($samples | Where-Object { $_.fix -eq "1" })
  $firstFix = $samples | Where-Object { $_.fix -eq "1" } | Select-Object -First 1
  $ttff = if ($firstFix) { [int]$firstFix.t_s - [int]$first.t_s } else { $null }
  $losses = 0; $longest = 0; $lossStart = $null; $gaps = 0; $maxGap = 0; $dist = 0.0; $prev = $null
  for ($i = 0; $i -lt $samples.Count; $i++) {
    $s = $samples[$i]
    if ($i) {
      $dt = [int]$s.t_s - [int]$samples[$i - 1].t_s
      if ($dt -gt 2) { $gaps++; if ($dt -gt $maxGap) { $maxGap = $dt } }
      if ($samples[$i - 1].fix -eq "1" -and $s.fix -ne "1") { $losses++; $lossStart = [int]$s.t_s }
      if ($samples[$i - 1].fix -ne "1" -and $s.fix -eq "1" -and $null -ne $lossStart) {
        $longest = [math]::Max($longest, [int]$s.t_s - $lossStart); $lossStart = $null
      }
    }
    if ($s.fix -eq "1") {
      if ($prev) {
        $m = Get-Haversine ([double]$prev.lat) ([double]$prev.lon) ([double]$s.lat) ([double]$s.lon)
        $spd = & $num $s.speed_kmh
        if (($spd -and $spd -gt 2) -or $m -gt 20) { $dist += $m }
      }
      $prev = $s
    } else { $prev = $null }
  }
  $afterFix = if ($firstFix) { @($samples | Where-Object { [int]$_.t_s -ge [int]$firstFix.t_s }).Count } else { 0 }
  $fixPct = if ($afterFix) { 100.0 * $fixRows.Count / $afterFix } else { 0 }
  $stat = { param($rows, $col) $v = @($rows | ForEach-Object { & $num $_.$col } | Where-Object { $null -ne $_ }); if ($v.Count) { $v | Measure-Object -Minimum -Maximum -Average } else { $null } }
  $sats = & $stat $fixRows "sats_used"
  $hdop = & $stat $fixRows "hdop"
  $speed = & $stat $fixRows "speed_kmh"
  $rate = & $stat $samples "gps_sentences_per_s"
  $heap = & $stat $samples "heap_int_min_kb"
  $psram = & $stat $samples "psram_kb"
  $ui = & $stat $samples "loop_ui_max_ms"
  $svc = & $stat $samples "loop_svc_max_ms"
  $delta = { param($col) [int](& $num $last.$col) - [int](& $num $first.$col) }
  $badCrc = & $delta "gps_bad_crc"; $overflow = & $delta "gps_overflow"
  $touchErr = & $delta "touch_i2c_err"; $sdErr = & $delta "sd_err"; $sdDrop = & $delta "sd_dropped"
  $linkDown = @($samples | Where-Object { $_.state -eq "NO_SIGNAL" }).Count
  $evCount = @{}; foreach ($e in $events) { $evCount[$e.event] = 1 + [int]$evCount[$e.event] }
  $resets = @($events | Where-Object { $_.event -eq "RESET" })
  $duration = [int]$last.t_s - [int]$first.t_s

  $fails = @(); $warns = @()
  $crashes = @($resets | Where-Object { $_.detail -match "PANIC|WDT|BROWNOUT" })
  if ($crashes.Count) { $fails += "device crashed during the test: " + (($crashes | ForEach-Object { $_.detail }) -join " | ") }
  elseif ($resets.Count) { $warns += "device restarted during the test (power/reset): " + (($resets | ForEach-Object { $_.detail }) -join " | ") }
  if ($sdErr -or $sdDrop) { $fails += "SD write errors $sdErr / dropped $sdDrop bytes" }
  if ($heap -and $heap.Minimum -lt 30) { $fails += "internal RAM low-water $($heap.Minimum) KB" }
  if ($linkDown * 10 -gt $samples.Count) { $fails += "GPS receiver silent in $linkDown of $($samples.Count) samples" }
  if ($null -eq $ttff) { $warns += "no GPS fix during the test" }
  if ($losses) { $warns += "fix lost $losses time(s), longest $longest s" }
  if ($afterFix -and $fixPct -lt 95) { $warns += ("fix only {0:n1} % of the time after the first fix" -f $fixPct) }
  if ($ttff -gt 180 -and $first.fix -ne "1") { $warns += "time to first fix $ttff s" }
  if ($hdop -and $hdop.Average -gt 5) { $warns += ("average HDOP {0:n1}" -f $hdop.Average) }
  if ($touchErr) { $warns += "touch I2C errors $touchErr" }
  if ($gaps) { $warns += "$gaps sampling gap(s), longest $maxGap s (loop stalled or reset)" }
  if ($evCount["LOOP_STALL"]) { $warns += "$($evCount['LOOP_STALL']) loop stall event(s)" }
  if ($badCrc -or $overflow) { $warns += "GPS data errors: $badCrc bad checksums, $overflow overflows" }
  $restarts = if ($samples[0].PSObject.Properties["gps_restarts"]) { & $delta "gps_restarts" } else { $null }   # column since v0.0.4+
  if ($restarts) { $warns += "GPS module rebooted $restarts time(s) during the test (power/wiring; each reboot loses the fix)" }
  $verdict = if ($fails.Count) { "FAIL" } elseif ($warns.Count) { "WARN" } else { "PASS" }

  $fmt = { param($m, $f) if ($m) { "{0:$f} / {1:$f} / {2:$f}" -f $m.Minimum, $m.Average, $m.Maximum } else { "n/a" } }
  $r = New-Object System.Collections.Generic.List[string]
  $r.Add("NAV-1 field test $id")
  $r.Add(("  period        {0} -> {1}   ({2:hh\:mm\:ss})" -f $first.utc, $last.utc, [timespan]::FromSeconds($duration)))
  $r.Add("  firmware      " + $(if ($device) { $device.firmware } else { ($events | Where-Object event -eq "START" | Select-Object -First 1).detail }))
  $r.Add("  samples       $($samples.Count) ($gaps gaps, longest $maxGap s)")
  $r.Add("  GPS")
  $r.Add("    first fix   " + $(if ($null -eq $ttff) { "never" } elseif ($first.fix -eq "1") { "fix at start" } else { "$ttff s after start" }))
  $r.Add(("    fix         {0} samples = {1:n1} % after the first fix; {2} loss(es), longest {3} s" -f $fixRows.Count, $fixPct, $losses, $longest))
  $r.Add("    satellites  " + (& $fmt $sats "n1") + "  (min / avg / max, while fixed)")
  $r.Add("    HDOP        " + (& $fmt $hdop "n2"))
  $r.Add(("    speed       max {0:n1} km/h, distance {1:n2} km" -f $(if ($speed) { $speed.Maximum } else { 0 }), ($dist / 1000)))
  $r.Add("    NMEA rate   " + (& $fmt $rate "n1") + " sentences/s; bad checksums $badCrc, overflows $overflow; receiver silent $linkDown s")
  $r.Add("    GPS module  " + $(if ($null -eq $restarts) { "restarts not recorded (older firmware)" } else { "$restarts restart(s)" }))
  $r.Add("  System")
  $r.Add("    RAM         internal low-water " + $(if ($heap) { "$($heap.Minimum) KB" } else { "n/a" }) + ", PSRAM min " + $(if ($psram) { "$($psram.Minimum) KB" } else { "n/a" }))
  $r.Add("    loop max    UI " + $(if ($ui) { "$($ui.Maximum) ms" } else { "n/a" }) + ", services " + $(if ($svc) { "$($svc.Maximum) ms" } else { "n/a" }))
  $r.Add("    errors      touch I2C $touchErr, SD write $sdErr, SD dropped $sdDrop bytes, resets $($resets.Count)")
  $r.Add("    radios      Wi-Fi $($last.wifi), BLE clients $($last.ble_clients)")
  $r.Add("  Events        " + (($evCount.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name) $($_.Value)" }) -join ", "))
  foreach ($e in ($events | Where-Object { $_.event -notin "START", "STOP", "WIFI", "BLE" } | Select-Object -First 25)) {
    $r.Add(("    t={0,6}s {1,-13} {2}" -f $e.t_s, $e.event, $e.detail))
  }
  if ($device) { $r.Add("  Device verdict: $($device.verdict)") }
  $r.Add("  VERDICT: $verdict")
  foreach ($f in $fails) { $r.Add("    FAIL  $f") }
  foreach ($w in $warns) { $r.Add("    WARN  $w") }

  $r | Set-Content (Join-Path $Dir "report.txt")
  [ordered]@{
    id = $id; verdict = $verdict; fail = $fails; warn = $warns; duration_s = $duration; samples = $samples.Count
    ttff_s = $ttff; fix_pct = [math]::Round($fixPct, 1); fix_losses = $losses; longest_loss_s = $longest
    sats = $sats; hdop = $hdop; max_speed_kmh = $(if ($speed) { $speed.Maximum } else { 0 }); distance_km = [math]::Round($dist / 1000, 3)
    gps_bad_checksums = $badCrc; gps_overflows = $overflow; touch_i2c_errors = $touchErr; sd_errors = $sdErr; sd_dropped = $sdDrop
    heap_low_kb = $(if ($heap) { $heap.Minimum } else { $null }); resets = $resets.Count; sampling_gaps = $gaps; events = $evCount
  } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Dir "analysis.json")
  $color = @{ PASS = "Green"; WARN = "Yellow"; FAIL = "Red" }[$verdict]
  foreach ($line in $r) { if ($line -like "  VERDICT*") { Write-Host $line -ForegroundColor $color } else { Write-Host $line } }
  Write-Host "  files: $Dir"
}

# ---------------------------------------------------------------------------------------------
if ($Local) { Invoke-Analysis (Join-Path $store $Local); exit 0 }

if ($From) {
  $src = Resolve-Path $From
  $name = Split-Path $src -Leaf
  if (-not (Test-Path (Join-Path $src "samples.csv"))) {          # a FIELD folder: take the newest test
    $name = (Get-ChildItem $src -Directory -Filter "FT-*" | Sort-Object LastWriteTime | Select-Object -Last 1).Name
    $src = Join-Path $src $name
  }
  $dst = Join-Path $store $name
  New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $src "*") $dst -Force
  Invoke-Analysis $dst
  exit 0
}

Open-Nav $Port
try {
  $st = Invoke-Nav "ft status" 1
  if ($st -match "\[FT\] running (\S+):") {
    if ($Stop) { [void](Invoke-Nav "ft stop" 4); Write-Host "stopped $($Matches[1])" }
    else { Write-Host "note: test $($Matches[1]) is still running (downloading what is recorded so far; -Stop to stop it first)" -ForegroundColor Yellow }
  }
  Send-Nav "ft list"
  $ls = Wait-NavFor "\[FT\] list end" 10
  $tests = @([regex]::Matches($ls, "^\s+(FT-[\w-]+)", "Multiline") | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
  if ($List) { $tests | ForEach-Object { Write-Host "  $_" }; exit 0 }
  if (-not $tests.Count) { Write-Host "no field tests on the SD card" -ForegroundColor Yellow; exit 1 }
  if (-not $Id) {
    if ($st -match "\[FT\] (running|not running, last test) (FT-[\w-]+)") { $Id = $Matches[2] }
    else {
      $dated = @($tests | Where-Object { $_ -match "^FT-\d{8}-\d{6}$" })
      $Id = if ($dated.Count) { $dated[-1] } else { $tests[-1] }
    }
  }
  $dst = Join-Path $store $Id
  New-Item -ItemType Directory -Force $dst | Out-Null
  foreach ($f in "summary.json", "events.csv", "samples.csv") {
    Write-Host "  downloading /FIELD/$Id/$f ..."
    $content = Get-DeviceFile "/FIELD/$Id/$f"
    if ($null -ne $content) { [IO.File]::WriteAllText((Join-Path $dst $f), $content) }
    elseif ($f -ne "summary.json") { throw "missing /FIELD/$Id/$f" }
    else { Write-Host "    (no summary.json: test still running or interrupted)" -ForegroundColor DarkGray }
  }
} finally { Close-Nav }
Invoke-Analysis $dst
