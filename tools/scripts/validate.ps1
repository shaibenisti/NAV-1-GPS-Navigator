<#
  validate.ps1 - one-command NAV-1 validation after every change.

    build -> flash -> reset + boot -> UI smoke test (+ RAM leak check, open timings)
          -> on-device self-test -> baseline drift check -> crash scan
          -> PASS / WARN / FAIL summary + JSON

  Examples:
    .\tools\scripts\validate.ps1                    # full run (~1.5 min incl. build)
    .\tools\scripts\validate.ps1 -NoBuild           # test the firmware already on the device
    .\tools\scripts\validate.ps1 -Only gps,wifi     # quick focused check (no build, no reboot, seconds)
    .\tools\scripts\validate.ps1 -Only ui           # just the UI smoke test
    .\tools\scripts\validate.ps1 -UpdateBaseline    # accept this run's measurements as the new baseline
    .\tools\scripts\validate.ps1 -Archive           # keep binaries + log + JSON in archive\<build id>\ (for tags)

  Builds with idf-build.ps1 (ESP-IDF).

  -Only areas: system memory gps time touch display sd wifi ble ui wifisave trips web sdfiles ota   (comma separated; web = PC fetches the NAV-1 web page, API, GPX;
              sdfiles = settings.json edited on the PC + SD icon/wallpaper, restored afterwards;
              ota = firmware update over Wi-Fi: refusals, rollback after a reset, install + verify - only with -Only ota)
  trips = replay a recorded outdoor NMEA session (baseline.json replay_file) into a trip, check the GPX
  wifisave = Wi-Fi persistence regression; needs tools\scripts\wifi.local.json {"ssid":..,"password":..}
  (git-ignored). A full run includes it automatically when that file exists.
  Exit code: 0 = no FAIL, 1 = at least one FAIL.
  Output: tools\scripts\logs\validate-<time>.log / .json, and logs\last.json (latest run).
  Device side: Diag::selfTest() prints "[CHECK] <area> <PASS|WARN|FAIL> <detail>" and
  "[METRICS] key=value ..."; the shell prints "[UI] visible <screen> <ms> ms".
#>
param(
  [string]$Port = "COM3",
  [switch]$NoBuild,
  [string]$Only = "",
  [switch]$Reset,
  [int]$BootTimeout = 30,
  [int]$UiCycles = 3,
  [switch]$UpdateBaseline,
  [switch]$Archive
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
. "$PSScriptRoot\NavSerial.ps1"

$quick = $Only -ne ""
$areas = @($Only -split "[,\s]+" | Where-Object { $_ })
$deviceAreas = ($areas | Where-Object { $_ -notin "ui", "wifisave", "trips", "web", "sdfiles", "ota" }) -join ","
$baseline0 = if (Test-Path "$PSScriptRoot\baseline.json") { Get-Content "$PSScriptRoot\baseline.json" -Raw | ConvertFrom-Json } else { $null }
# Wi-Fi persistence regression (forget -> wrong password -> connect -> reboot -> auto-reconnect).
# Needs the real network: tools\scripts\wifi.local.json = { "ssid": "...", "password": "..." } (git-ignored).
$wifiFile = Join-Path $PSScriptRoot "wifi.local.json"
$wifiCreds = if (Test-Path $wifiFile) { Get-Content $wifiFile -Raw | ConvertFrom-Json } else { $null }
$runWifiSave = ($areas -contains "wifisave") -or ((-not $quick) -and $wifiCreds)
$runUi = (-not $quick) -or ($areas -contains "ui")
$runSelftest = (-not $quick) -or ($deviceAreas -ne "")
$doBuild = -not ($NoBuild -or $quick)
$doReset = $doBuild -or $Reset -or (-not $quick)

$results = New-Object System.Collections.Generic.List[object]
$metrics = [ordered]@{}
function Add-Result([string]$Area, [string]$Status, [string]$Detail) {
  $results.Add([pscustomobject]@{ area = $Area; status = $Status; detail = $Detail })
}
function Get-InternalFreeKB([string]$text) {
  if ($text -match "\[mem\] internal free (\d+) KB") { return [int]$Matches[1] }
  return $null
}

$t0 = Get-Date
$logDir = Join-Path $PSScriptRoot "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
$stamp = "{0:yyyyMMdd-HHmmss}" -f $t0
$logFile = Join-Path $logDir "validate-$stamp.log"
$jsonFile = Join-Path $logDir "validate-$stamp.json"
$commit = (git -C $root rev-parse --short HEAD 2>$null)
$gitDescribe = (git -C $root describe --tags --always --dirty 2>$null)
$buildLog = ""
$archiveDir = ""
$otaImage = ""

# ---- 1. build + flash ---------------------------------------------------------------------------
if ($doBuild) {
  Write-Host "[1/5] build + flash ($Port, $gitDescribe)..." -ForegroundColor Cyan
  $buildArgs = @{ Upload = $true; Port = $Port; OutDir = (Join-Path $root "build\validate") }   # image for the ota area
  if ($Archive) {
    if ($gitDescribe -match "dirty|-g[0-9a-f]+$") { Write-Host "  note: archiving a build that is not exactly a tag ($gitDescribe)" -ForegroundColor Yellow }
    $archiveDir = Join-Path $root "archive\$gitDescribe"
    $buildArgs.OutDir = $archiveDir
  }
  $otaImage = Join-Path $buildArgs.OutDir "NAV1.bin"
  $buildScript = Join-Path $root "idf-build.ps1"
  $ok = $true
  try { $buildLog = & $buildScript @buildArgs *>&1 | Out-String }
  catch { $ok = $false; $buildLog += "`n$_" }
  # idf-build.ps1 prints "firmware N bytes = P % of the 3 MB app slot"
  if ($buildLog -match "firmware (\d+) bytes = (\d+) %") {
    $metrics.firmware_kb = [math]::Round([int]$Matches[1] / 1KB)
    Add-Result "build" ($(if ([int]$Matches[2] -lt 85) { "PASS" } else { "WARN" })) "firmware $($metrics.firmware_kb) KB = $($Matches[2]) % of the 3 MB app partition"
  }
  $warn = if ($buildLog -match "project warnings: (\d+)") { [int]$Matches[1] } else { ([regex]::Matches($buildLog, "firmware\\[^\r\n]*warning")).Count }
  Add-Result "build" ($(if ($warn -eq 0) { "PASS" } else { "WARN" })) "$warn compiler warning(s) in project code"
  if (-not $ok -or $buildLog -notmatch "(?i)Hash of data verified") {
    Add-Result "flash" "FAIL" "build or upload failed (see log)"
    Set-Content $logFile $buildLog
    $results | Format-Table -AutoSize
    Write-Host "FAIL - log: $logFile" -ForegroundColor Red
    exit 1
  }
  Add-Result "flash" "PASS" "uploaded and verified"
} else {
  Write-Host "[1/5] build skipped" -ForegroundColor DarkGray
}

$firmware = ""
Open-Nav $Port
try {
  # ---- 2. boot ----------------------------------------------------------------------------------
  if ($doReset) {
    Write-Host "[2/5] reset + boot..." -ForegroundColor Cyan
    Reset-Nav
    $boot = Wait-NavFor "\[SHELL\] ready[^\r\n]*\r?\n" $BootTimeout
    if (-not $boot) {
      Add-Result "boot" "FAIL" "no 'ready' line within $BootTimeout s"
    } else {
      Wait-Nav 3                                          # let services settle
      $boot = Get-NavLog
      if ($boot -match "NAV-1\s+v(\S+)\s+\(([^,]+),") { $firmware = $Matches[2]; Add-Result "boot" "PASS" "NAV-1 v$($Matches[1]) ($firmware)" }
      Add-Result "boot" ($(if ($boot -match "Board config: OK") { "PASS" } else { "FAIL" })) "board config (flash/PSRAM)"
      $corrupt = ([regex]::Matches($boot, "heap integrity after .*: CORRUPT")).Count
      Add-Result "boot" ($(if ($corrupt -eq 0) { "PASS" } else { "FAIL" })) "heap integrity during start-up ($corrupt corrupt)"
      if ($boot -match "ready in (\d+) ms: ([^\r\n]*)") { $metrics.boot_ms = [int]$Matches[1]; Add-Result "boot" "PASS" "ready in $($Matches[1]) ms: $($Matches[2])" }
    }
  } else {
    Write-Host "[2/5] no reboot (quick check on the running firmware)" -ForegroundColor DarkGray
    if ((Invoke-Nav "diag sys" 0.6) -match "\[sys\] NAV-1 v\S+ \(([^)]+)\)") { $firmware = $Matches[1] }
  }

  # ---- 3. UI smoke test: open every app, RAM leak check, open timings -----------------------------
  $uiApps = "GPS", "Trips", "Phone", "Files", "Settings", "Tools"   # every implemented app
  if ($runUi) {
    Write-Host "[3/5] UI smoke test ($UiCycles cycles)..." -ForegroundColor Cyan
    $before = Get-InternalFreeKB (Invoke-Nav "diag mem" 0.6)
    $opened = 0
    $uiText = ""
    for ($i = 0; $i -lt $UiCycles; $i++) {
      foreach ($app in $uiApps) {
        $r = Invoke-Nav "open $app" 2.2
        $uiText += $r
        if ($r -match "\[UI\] opened $app") { $opened++ }
      }
      $uiText += Invoke-Nav "home" 1.2
    }
    Wait-Nav 1
    $after = Get-InternalFreeKB (Invoke-Nav "diag mem" 0.6)
    $expected = $uiApps.Count * $UiCycles
    Add-Result "ui" ($(if ($opened -eq $expected) { "PASS" } else { "FAIL" })) "opened $opened / $expected app screens ($($uiApps -join ', '))"
    $appMs = @([regex]::Matches($uiText, "\[UI\] visible (?!Home)\S+ (\d+) ms") | ForEach-Object { [int]$_.Groups[1].Value })
    $homeMs = @([regex]::Matches($uiText, "\[UI\] visible Home (\d+) ms") | ForEach-Object { [int]$_.Groups[1].Value })
    if ($appMs.Count) {
      $metrics.app_open_ms = ($appMs | Measure-Object -Maximum).Maximum
      Add-Result "ui" "PASS" ("app open visible after {0}-{1} ms (avg {2:n0}), Home {3}-{4} ms" -f ($appMs | Measure-Object -Minimum).Minimum,
        $metrics.app_open_ms, ($appMs | Measure-Object -Average).Average, ($homeMs | Measure-Object -Minimum).Minimum, ($homeMs | Measure-Object -Maximum).Maximum)
    }
    if ($homeMs.Count) { $metrics.home_ms = ($homeMs | Measure-Object -Maximum).Maximum }
    if ($null -ne $before -and $null -ne $after) {
      $lost = $before - $after
      $st = if ($lost -le 3) { "PASS" } elseif ($lost -le 8) { "WARN" } else { "FAIL" }
      Add-Result "memory" $st "internal RAM $before KB -> $after KB after $UiCycles UI cycles (leak check)"
    }
  }

  # ---- 4. on-device self-test ---------------------------------------------------------------------
  if ($runSelftest) {
    Write-Host ("[4/5] device self-test ({0})..." -f $(if ($deviceAreas) { $deviceAreas } else { "all, ~20 s" })) -ForegroundColor Cyan
    Send-Nav ("selftest " + $deviceAreas).Trim()
    $st = Wait-NavFor "\[SELFTEST\] done[^\r\n]*" 90
    if (-not $st) {
      Add-Result "selftest" "FAIL" "no result within 90 s"
    } else {
      foreach ($m in [regex]::Matches($st, "\[CHECK\]\s+(\S+)\s+(PASS|WARN|FAIL)\s+([^\r\n]*)")) {
        Add-Result $m.Groups[1].Value $m.Groups[2].Value $m.Groups[3].Value
      }
      if ($st -match "\[METRICS\]([^\r\n]*)") {
        foreach ($kv in [regex]::Matches($Matches[1], "(\w+)=(\d+)")) {
          $k = $kv.Groups[1].Value
          if ($k -eq "boot_ms" -and -not $metrics.Contains("boot_ms")) { continue }   # only from a fresh boot
          if (-not $metrics.Contains($k)) { $metrics[$k] = [int]$kv.Groups[2].Value }
        }
      }
    }
    if (-not $quick) { foreach ($s in "diag gps", "diag wifi", "diag ble", "diag display", "diag sd") { [void](Invoke-Nav $s 0.5) } }
  }

  # ---- 4a. Trips regression: replay a recorded outdoor session into a trip -------------------------
  if ($areas -contains "trips") {
    Write-Host "[4a] Trips regression (GPS replay, ~40 s)..." -ForegroundColor Cyan
    $replay = if ($baseline0 -and $baseline0.replay_file) { $baseline0.replay_file } else { "/GPSLOG/S0165.NMEA" }
    $r = Invoke-Nav "gps replay $replay 20" 1
    if ($r -match "cannot start") {
      Add-Result "trips" "WARN" "skipped: replay file $replay not on the SD card"
    } else {
      [void](Invoke-Nav "trip start" 2)
      Wait-Nav $(if ($baseline0 -and $baseline0.replay_wait_s) { $baseline0.replay_wait_s } else { 32 })
      [void](Invoke-Nav "trip stop" 3)
      $l = Invoke-Nav "trip list" 3
      [void](Invoke-Nav "gps live" 1)
      $first = [regex]::Match($l, "\[TRIP\] (\S+)\s+.*?([\d.]+) km\s+(\d+) s.*?(\d+) points")
      $ok = $first.Success -and [int]$first.Groups[4].Value -gt 0
      Add-Result "trips" ($(if ($ok) { "PASS" } else { "FAIL" })) $(if ($first.Success) { "recorded $($first.Groups[1].Value): $($first.Groups[4].Value) points, $($first.Groups[2].Value) km, $($first.Groups[3].Value) s (replay $replay at 20x)" } else { "no trip in the list" })
      # Clock not set (no Wi-Fi; replayed GPS time is ignored) -> /data/trips/undated/TRIP-Nnnnn
      $tripName = $first.Groups[1].Value
      $tripDir = if ($tripName -match '^\d{4}') { $tripName.Substring(0, 4) } else { "undated" }
      $g = Invoke-Nav "sd cat /data/trips/$tripDir/$tripName.gpx" 3
      Add-Result "trips" ($(if ($g -match "<trkpt" -and $g -match "</gpx>") { "PASS" } else { "FAIL" })) "GPX file has track points and is closed"
      # The summary distance must be the GPX track length (2026-09-29: 1.09 km shown for a 0.58 km walk)
      $pts = [regex]::Matches($g, 'lat="([-\d.]+)" lon="([-\d.]+)"')
      $trk = 0.0
      for ($i = 1; $i -lt $pts.Count; $i++) {
        $la1 = [double]$pts[$i - 1].Groups[1].Value * [math]::PI / 180; $la2 = [double]$pts[$i].Groups[1].Value * [math]::PI / 180
        $dla = $la2 - $la1; $dlo = ([double]$pts[$i].Groups[2].Value - [double]$pts[$i - 1].Groups[2].Value) * [math]::PI / 180
        $h = [math]::Sin($dla / 2) * [math]::Sin($dla / 2) + [math]::Cos($la1) * [math]::Cos($la2) * [math]::Sin($dlo / 2) * [math]::Sin($dlo / 2)
        $trk += 2 * 6371000 * [math]::Asin([math]::Sqrt($h))
      }
      $sum = if ($first.Success) { [double]$first.Groups[2].Value * 1000 } else { -1 }
      $okD = $trk -gt 50 -and [math]::Abs($sum - $trk) -le [math]::Max(10, 0.05 * $trk)
      Add-Result "trips" ($(if ($okD) { "PASS" } else { "FAIL" })) ("distance {0:N0} m = GPX track {1:N0} m ({2} points)" -f $sum, $trk, $pts.Count)
    }
  }

  # ---- 4c. Web service (iPhone link) checked from the PC over the LAN ------------------------------
  if ($areas -contains "web" -or -not $quick) {
    $w = Invoke-Nav "diag wifi" 1
    if ($w -notmatch "web: http://([\d.]+)/") {
      Add-Result "web" $(if ($w -match "\[wifi\] connected") { "FAIL" } else { "WARN" }) "web server not running (Wi-Fi connected: $($w -match '\[wifi\] connected'))"
    } else {
      $ip = $Matches[1]
      Write-Host "[4c] web service at http://$ip/ ..." -ForegroundColor Cyan
      try {
        $s = (Invoke-WebRequest "http://$ip/api/status" -TimeoutSec 8).Content | ConvertFrom-Json
        $l = (Invoke-WebRequest "http://$ip/api/location" -TimeoutSec 8).Content | ConvertFrom-Json
        Add-Result "web" "PASS" "/api/status + /api/location: $($s.name), $($s.firmware), GPS $($l.state)"
        $page = Invoke-WebRequest "http://$ip/" -TimeoutSec 8
        Add-Result "web" ($(if ($page.Content -match "<title>NAV-1</title>") { "PASS" } else { "FAIL" })) "web page ($($page.RawContentLength) bytes)"
        $t = @((Invoke-WebRequest "http://$ip/api/trips" -TimeoutSec 15).Content | ConvertFrom-Json)
        if ($t.Count) {
          $sw = [Diagnostics.Stopwatch]::StartNew()
          $g = Invoke-WebRequest "http://$ip$($t[0].gpx)" -TimeoutSec 30
          $okXml = try { [xml]$g.Content | Out-Null; $true } catch { $false }
          Add-Result "web" ($(if ($okXml) { "PASS" } else { "FAIL" })) "/api/trips ($($t.Count) trips) + GPX download $($g.RawContentLength) bytes in $($sw.ElapsedMilliseconds) ms, valid XML"
        } else { Add-Result "web" "WARN" "/api/trips: no trips to download" }
        $code = try { (Invoke-WebRequest "http://$ip/trips/../../system" -TimeoutSec 5).StatusCode } catch { $_.Exception.Response.StatusCode.value__ }
        Add-Result "web" ($(if ($code -eq 404) { "PASS" } else { "FAIL" })) "path escape attempt refused (HTTP $code)"
      } catch { Add-Result "web" "FAIL" "request failed: $($_.Exception.Message)" }
    }
  }

  # ---- 4d. SD files: settings.json edited on a PC + images (M4) ------------------------------------
  if ($areas -contains "sdfiles" -or -not $quick) {
    $dir = Invoke-Nav "sd dir /assets" 1
    $orig = Invoke-Nav "sd cat /system/settings.json" 1.5
    if ($dir -match "dir /assets: [1-9]") {
      Add-Result "sdfiles" "WARN" "skipped: the card has its own /assets (not overwritten by the test)"
    } elseif ($orig -notmatch "(?s)<<<BEGIN /system/settings.json \d+>>>\r?\n(.*?)<<<END") {
      Add-Result "sdfiles" "FAIL" "no /system/settings.json on the card"
    } else {
      Write-Host "[4d] SD files: settings.json from the PC + images (~30 s)..." -ForegroundColor Cyan
      $origBytes = [Text.Encoding]::UTF8.GetBytes(($Matches[1] -replace "`r", "").TrimEnd("`n") + "`n")
      $td = Join-Path $PSScriptRoot "testdata"
      try {
        Send-NavFile ([Text.Encoding]::UTF8.GetBytes("{`n  `"time_zone`": `"UTC`",`n  `"device_name`": `"bad name!`"`n}`n")) "/system/settings.json"
        Send-NavFile ([IO.File]::ReadAllBytes("$td\Notes.bin")) "/assets/icons/Notes.bin"
        Send-NavFile ([IO.File]::ReadAllBytes("$td\home.bin")) "/assets/wallpapers/home.bin"
        Send-NavFile ([byte[]](1..40)) "/assets/icons/Alerts.bin"
        Reset-Nav
        $boot = Wait-NavFor "\[SHELL\] ready[^\r\n]*" $BootTimeout
        $tm = Invoke-Nav "diag time" 1
        $ok = $boot -match "\[SET\] /system/settings.json: 1 value\(s\) applied, 1 invalid ignored" -and $tm -match "zone UTC|\(UTC\)"
        Add-Result "sdfiles" ($(if ($ok) { "PASS" } else { "FAIL" })) "settings.json edited on the PC: time zone applied at boot, invalid device name ignored"
        $ok = $boot -match "\[ASSET\] 2 loaded, 1 ignored" -and $boot -match "Alerts.bin ignored: not an LVGL"
        Add-Result "sdfiles" ($(if ($ok) { "PASS" } else { "FAIL" })) "SD images: tile icon + wallpaper loaded, corrupt file ignored (built-in icon kept)"
        $sw = Invoke-Nav "swipe" 1.5
        Add-Result "sdfiles" ($(if ($sw -match "frame\s+1 stale=0") { "PASS" } else { "FAIL" })) "home page with the SD icon draws ($(if ($sw -match '\+\s*(\d+)ms frame') { $Matches[1] } else { '?' }) ms)"
        [void](Invoke-Nav "swipe" 1)
      } catch { Add-Result "sdfiles" "FAIL" "$_" }
      finally {                                    # always restore: the user's settings, no test images
        try { Send-NavFile $origBytes "/system/settings.json" } catch { Add-Result "sdfiles" "FAIL" "could not restore settings.json: $_" }
        $rm = Invoke-Nav "sd rm /assets" 5
        Reset-Nav
        $boot = Wait-NavFor "\[SHELL\] ready[^\r\n]*" $BootTimeout
        $ok = $rm -match "rm /assets: ok" -and $boot -notmatch "\[ASSET\]" -and $boot -match "\[SET\] /system/settings.json: 1 value\(s\) applied\r?\n"
        Add-Result "sdfiles" ($(if ($ok) { "PASS" } else { "FAIL" })) "restored: original settings applied again, test images removed (built-in look)"
      }
    }
  }

  # ---- 4e. OTA: refusals, rollback after a reset, install + self-verification (M9) ------------------
  # Only on request (-Only ota): it installs the firmware twice and resets the device in between; the
  # it is not part of routine checks - the screen goes dark twice during the installs.
  if ($areas -contains "ota") {
    if (-not $otaImage) { $otaImage = Join-Path $root "build\validate\NAV1.bin" }
    $end = (Get-Date).AddSeconds(30)             # may follow a reboot (sdfiles): Wi-Fi reconnects in a few s
    do { $w = Invoke-Nav "diag wifi" 1 } while ($w -notmatch "connected '.*?' ip ([\d.]+)" -and (Get-Date) -lt $end)
    if ($w -notmatch "connected '.*?' ip ([\d.]+)") { Add-Result "ota" "WARN" "skipped: NAV-1 not on Wi-Fi" }
    elseif (-not (Test-Path $otaImage)) { Add-Result "ota" "WARN" "skipped: no firmware image ($otaImage) - run a full validate once" }
    else {
      $ip = $Matches[1]
      Write-Host "[4e] OTA over Wi-Fi: refusals, rollback, install (~2 min)..." -ForegroundColor Cyan
      function Send-Ota([string]$file) {
        try { $r = Invoke-RestMethod "http://$ip/api/update" -Method Post -Form @{ file = Get-Item $file } -TimeoutSec 180; return "200 $($r.message)" }
        catch { return "$($_.Exception.Response.StatusCode.value__) $($_.ErrorDetails.Message)" }
      }
      function Get-Slot { if ((Invoke-Nav "ota" 1) -match "slot (app\d)") { return $Matches[1] } }
      [void](Invoke-Nav "ota disarm" 0.5)
      $r1 = Send-Ota $otaImage
      [void](Invoke-Nav "ota arm" 0.5)
      $r2 = Send-Ota (Join-Path $PSScriptRoot "testdata\home.bin")
      Add-Result "ota" ($(if ($r1 -match "^403" -and $r2 -match "(?s)^400.*not an ESP32-S3") { "PASS" } else { "FAIL" })) "refused: upload while not armed (403), non-firmware file (400)"
      $slot0 = Get-Slot
      [void](Invoke-Nav "ota arm" 0.5)
      $sw = [Diagnostics.Stopwatch]::StartNew()
      $r3 = Send-Ota $otaImage
      $upS = $sw.Elapsed.TotalSeconds
      $boot = Wait-NavFor "\[OTA\] running [^\r\n]*\r?\n" 40   # the whole line (a half-received one failed the check)
      $pending = $r3 -match "^200" -and $boot -match "NEW firmware: verifying" -and $boot -notmatch "running $slot0"
      Reset-Nav                                    # power loss before the 20 s verification
      $b2 = Wait-NavFor "\[SHELL\] ready[^\r\n]*" $BootTimeout
      $back = (Get-Slot) -eq $slot0
      Add-Result "ota" ($(if ($pending -and $back) { "PASS" } else { "FAIL" })) ("install ({0:N0} s upload) + reset during verification -> rolled back to {1}" -f $upS, $slot0)
      [void](Invoke-Nav "ota arm" 0.5)
      $r4 = Send-Ota $otaImage
      $v = Wait-NavFor "\[OTA\] new firmware verified[^\r\n]*" 70
      $slot1 = Get-Slot
      Add-Result "ota" ($(if ($r4 -match "^200" -and $v -and $slot1 -ne $slot0) { "PASS" } else { "FAIL" })) "install -> reboot -> verified after 20 s and kept ($slot0 -> $slot1)"
    }
  }

  # ---- 4b. Wi-Fi persistence regression ------------------------------------------------------------
  if ($runWifiSave) {
    if (-not $wifiCreds) {
      Add-Result "wifisave" "WARN" "skipped: create tools\scripts\wifi.local.json with the home network ssid/password"
    } else {
      Write-Host "[4b] Wi-Fi persistence regression (~1.5 min)..." -ForegroundColor Cyan
      $ssid = $wifiCreds.ssid
      function Wait-Wifi([string]$pattern, [int]$seconds) {
        $end = (Get-Date).AddSeconds($seconds)
        while ((Get-Date) -lt $end) { $w = Invoke-Nav "diag wifi" 0.8; if ($w -match $pattern) { return $w }; Start-Sleep -Milliseconds 1500 }
        return $null
      }
      [void](Invoke-Nav "wifi forget" 1)
      $w = Invoke-Nav "diag wifi" 0.8
      Add-Result "wifisave" ($(if ($w -match "saved network: none") { "PASS" } else { "FAIL" })) "Forget clears the saved network"
      [void](Invoke-Nav "wifi connect ""$ssid"" wrong-password-regression" 1)
      $w = Wait-Wifi "last error: (wrong password|could not|timed out)" 45
      $ok = $w -and $w -match "saved network: none"
      Add-Result "wifisave" ($(if ($ok) { "PASS" } else { "FAIL" })) "wrong password is rejected and not saved"
      [void](Invoke-Nav "wifi connect ""$ssid"" ""$($wifiCreds.password)""" 1)
      $w = Wait-Wifi "\[wifi\] connected" 45
      $ok = $w -and $w -match "saved network '$([regex]::Escape($ssid))'"
      Add-Result "wifisave" ($(if ($ok) { "PASS" } else { "FAIL" })) "correct password connects and is saved (read back from NVS)"
      Reset-Nav
      $boot = Wait-NavFor "\[SHELL\] ready[^\r\n]*" $BootTimeout
      $t = Get-Date
      $w = Wait-Wifi "\[wifi\] connected" 60
      Add-Result "wifisave" ($(if ($w) { "PASS" } else { "FAIL" })) ("after a reboot it reconnects automatically" + $(if ($w) { " ({0:n0} s after ready)" -f ((Get-Date) - $t).TotalSeconds } else { "" }))
    }
  }
} finally {
  Close-Nav
}

# ---- 5. baseline drift, crash scan, summary ---------------------------------------------------------
$baselineFile = Join-Path $PSScriptRoot "baseline.json"
$baseline = if (Test-Path $baselineFile) { Get-Content $baselineFile -Raw | ConvertFrom-Json } else { $null }
if ($baseline) {
  foreach ($name in $metrics.Keys) {
    $b = $baseline.metrics.$name
    if (-not $b) { continue }
    $v = [double]$metrics[$name]
    $delta = if ($b.worse -eq "up") { $v - $b.value } else { $b.value - $v }   # > 0 = worse
    $pct = if ($b.value) { 100.0 * $delta / $b.value } else { 0 }
    $st = if ($delta -lt $b.min_abs) { "PASS" } elseif ($pct -ge $b.fail_pct) { "FAIL" } elseif ($pct -ge $b.warn_pct) { "WARN" } else { "PASS" }
    Add-Result "baseline" $st ("{0} = {1} (baseline {2}, {3:+0;-0} %)" -f $name, $v, $b.value, $(if ($b.worse -eq "up") { $pct } else { -$pct }))
  }
}

$serial = Get-NavLog
# A software reset right after "[OTA] rebooting / rejecting" is the update's own restart, not a crash.
$serial = [regex]::Replace($serial, "(?s)(\[OTA\] (rebooting into the new firmware|rejecting the new firmware)[^\n]*\n.{0,400}?)rst:0xc \(RTC_SW_CPU_RST\)", '$1(OTA restart)')
# ESP-IDF arms the RTC watchdog as a safety net during esp_restart(); on this board it sometimes
# fires in the ROM stage right after the restart (rst:0x10), then the boot is normal (seen 2026-09-29).
$serial = [regex]::Replace($serial, "(?s)(\(OTA restart\).{0,300}?)rst:0x10 \(RTCWDT_RTC_RST\)", '$1(OTA restart, RTC WDT)')
# ... or it comes straight after the "rebooting" line, without the software-reset line first (v0.2.2 check).
$serial = [regex]::Replace($serial, "(?s)(\[OTA\] (rebooting into the new firmware|rejecting the new firmware)[^\n]*\n.{0,400}?)rst:0x10 \(RTCWDT_RTC_RST\)", '$1(OTA restart, RTC WDT)')
$crash = [regex]::Matches($serial, "Guru Meditation|panic'ed|abort\(\) was called|assert failed|Stack canary|Brownout|CORRUPT HEAP|rst:0x[0-9a-f]+ \((?!POWERON)")
Add-Result "stability" ($(if ($crash.Count -eq 0) { "PASS" } else { "FAIL" })) $(if ($crash.Count) { "crash signature: $($crash[0].Value)" } else { "no crash/reset signatures in the serial log" })

$pass = @($results | Where-Object status -eq "PASS").Count
$warnN = @($results | Where-Object status -eq "WARN").Count
$fail = @($results | Where-Object status -eq "FAIL").Count
$overall = if ($fail) { "FAIL" } elseif ($warnN) { "PASS_WITH_WARNINGS" } else { "PASS" }
$seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds)

Set-Content $logFile ($buildLog + "`n==================== SERIAL ====================`n" + $serial)
$summary = [ordered]@{
  timestamp = $t0.ToString("s"); mode = $(if ($quick) { "only:$Only" } else { "full" }); port = $Port
  git_commit = $commit; git_describe = $gitDescribe; firmware = $firmware
  overall = $overall; pass = $pass; warn = $warnN; fail = $fail; seconds = $seconds
  metrics = $metrics; results = $results; log = $logFile
}
$json = $summary | ConvertTo-Json -Depth 5
Set-Content $jsonFile $json
Set-Content (Join-Path $logDir "last.json") $json

if ($UpdateBaseline -and $baseline) {
  foreach ($name in $metrics.Keys) { if ($baseline.metrics.$name) { $baseline.metrics.$name.value = $metrics[$name] } }
  $baseline.firmware = $firmware
  $baseline | ConvertTo-Json -Depth 5 | Set-Content $baselineFile
  Write-Host "  baseline updated from this run ($firmware)" -ForegroundColor Cyan
}
if ($archiveDir) {
  Copy-Item $logFile, $jsonFile $archiveDir
  Write-Host "  archived: $archiveDir" -ForegroundColor Cyan
}

Write-Host "[5/5] summary" -ForegroundColor Cyan
Write-Host ""
foreach ($r in $results) {
  $color = switch ($r.status) { "PASS" { "Green" } "WARN" { "Yellow" } default { "Red" } }
  Write-Host ("  {0,-4}  " -f $r.status) -ForegroundColor $color -NoNewline
  Write-Host ("{0,-9} {1}" -f $r.area, $r.detail)
}
$color = if ($fail) { "Red" } elseif ($warnN) { "Yellow" } else { "Green" }
Write-Host ""
Write-Host ("  RESULT: {0}   ({1} pass, {2} warn, {3} fail, {4} s)   firmware {5}, commit {6}" -f $overall, $pass, $warnN, $fail, $seconds, $firmware, $commit) -ForegroundColor $color
Write-Host "  log: $logFile"
exit $(if ($fail) { 1 } else { 0 })
