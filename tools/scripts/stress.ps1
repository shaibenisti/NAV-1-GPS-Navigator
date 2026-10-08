<#
  stress.ps1 - repeat/soak tests for NAV-1 (run before tagging, or after risky changes).

  UI stress (default): opens and closes every app N times plus page swipes; samples
  internal RAM every 10 cycles and checks drift, open timings, touch state and crashes.
    .\tools\scripts\stress.ps1                   # 50 cycles (~6 min)
    .\tools\scripts\stress.ps1 -Cycles 200

  Reboot soak: restarts the device N times and checks every boot.
    .\tools\scripts\stress.ps1 -Reboots 20       # ~15 s per reboot

  Exit code 1 on any FAIL. Log: tools\scripts\logs\stress-<time>.log
#>
param(
  [string]$Port = "COM3",
  [int]$Cycles = 50,
  [int]$Reboots = 0
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\NavSerial.ps1"

$fails = New-Object System.Collections.Generic.List[string]
$warns = New-Object System.Collections.Generic.List[string]
$t0 = Get-Date
$logDir = Join-Path $PSScriptRoot "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
$logFile = Join-Path $logDir ("stress-{0:yyyyMMdd-HHmmss}.log" -f $t0)
function Get-InternalFreeKB([string]$text) {
  if ($text -match "\[mem\] internal free (\d+) KB") { return [int]$Matches[1] }
  return $null
}
function Stat([int[]]$v) { if (-not $v.Count) { return "n/a" }; $m = $v | Measure-Object -Minimum -Maximum -Average; "{0}/{1:n0}/{2} ms (min/avg/max)" -f $m.Minimum, $m.Average, $m.Maximum }

Open-Nav $Port
try {
  if ($Reboots -gt 0) {
    # ---- reboot soak -----------------------------------------------------------------------------
    $boots = @()
    for ($i = 1; $i -le $Reboots; $i++) {
      Reset-Nav
      $text = Wait-NavFor "\[SHELL\] ready in (\d+) ms" 30
      if (-not $text) { $fails.Add("reboot $i : no ready line within 30 s"); continue }
      [void]($text -match "\[SHELL\] ready in (\d+) ms"); $boots += [int]$Matches[1]
      if ($text -match "CORRUPT") { $fails.Add("reboot $i : heap integrity CORRUPT during start-up") }
      if ($text -notmatch "Board config: OK") { $fails.Add("reboot $i : board config not OK") }
      if ($text -match "SD NOT logging") { $warns.Add("reboot $i : SD not logging") }
      Write-Host ("  reboot {0,3}/{1}: ready in {2} ms" -f $i, $Reboots, $boots[-1])
      Wait-Nav 2
    }
    Write-Host ("  boot time " + (Stat $boots))
  } else {
    # ---- UI stress ---------------------------------------------------------------------------------
    [void](Invoke-Nav "home" 1)
    $start = Get-InternalFreeKB (Invoke-Nav "diag mem" 0.6)
    $samples = @($start)
    $text = ""
    for ($c = 1; $c -le $Cycles; $c++) {
      foreach ($app in "GPS", "Trips", "Phone", "Files", "Settings", "Tools", "Navigate", "Places", "Drive") { $text += Invoke-Nav "open $app" 1.6 }
      $text += Invoke-Nav "home" 0.9
      $text += Invoke-Nav "swipe" 0.9
      if ($c % 10 -eq 0) {
        $kb = Get-InternalFreeKB (Invoke-Nav "diag mem" 0.6)
        $samples += $kb
        Write-Host ("  cycle {0,4}/{1}: internal free {2} KB" -f $c, $Cycles, $kb)
      }
    }
    Wait-Nav 2
    $end = Get-InternalFreeKB (Invoke-Nav "diag mem" 0.6)
    $touch = Invoke-Nav "diag touch" 0.6
    $opened = ([regex]::Matches($text, "\[UI\] opened ")).Count
    if ($opened -ne 6 * $Cycles) { $fails.Add("opened $opened of $(6 * $Cycles) app screens") }
    $appMs = @([regex]::Matches($text, "\[UI\] visible (?!Home)\S+ (\d+) ms") | ForEach-Object { [int]$_.Groups[1].Value })
    $homeMs = @([regex]::Matches($text, "\[UI\] visible Home (\d+) ms") | ForEach-Object { [int]$_.Groups[1].Value })
    Write-Host ("  app open " + (Stat $appMs) + ", Home " + (Stat $homeMs))
    if ($appMs.Count -and ($appMs | Measure-Object -Maximum).Maximum -gt 1000) { $warns.Add("an app took over 1 s to appear") }
    $drift = $start - $end
    Write-Host "  internal RAM: start $start KB, end $end KB (drift $drift KB), samples: $($samples -join ', ')"
    if ($drift -gt 12) { $fails.Add("internal RAM dropped $drift KB over $Cycles cycles") }
    elseif ($drift -gt 4) { $warns.Add("internal RAM dropped $drift KB over $Cycles cycles") }
    if ($touch -match "i2c errors (\d+), overflows (\d+), stuck (\d+)") {
      if ([int]$Matches[1] -gt 0) { $warns.Add("touch I2C errors: $($Matches[1])") }
      if ([int]$Matches[2] -gt 0 -or [int]$Matches[3] -gt 0) { $warns.Add("touch overflows $($Matches[2]) / stuck releases $($Matches[3])") }
    }
    $st = Invoke-Nav "selftest memory,touch,display" 6
    if ($st -match "fail=([1-9]\d*)") { $fails.Add("self-test after stress: $($Matches[1]) fail(s)") }
  }
} finally {
  Close-Nav
}

$serial = Get-NavLog
Set-Content $logFile $serial
$crash = [regex]::Matches($serial, "Guru Meditation|panic'ed|abort\(\) was called|assert failed|Stack canary|Brownout|CORRUPT HEAP")
if ($crash.Count) { $fails.Add("crash signature: $($crash[0].Value)") }

foreach ($w in $warns) { Write-Host "  WARN  $w" -ForegroundColor Yellow }
foreach ($f in $fails) { Write-Host "  FAIL  $f" -ForegroundColor Red }
$result = if ($fails.Count) { "FAIL" } elseif ($warns.Count) { "PASS with warnings" } else { "PASS" }
$color = if ($fails.Count) { "Red" } elseif ($warns.Count) { "Yellow" } else { "Green" }
Write-Host ("  STRESS RESULT: {0} ({1:n0} s)  log: {2}" -f $result, ((Get-Date) - $t0).TotalSeconds, $logFile) -ForegroundColor $color
exit $(if ($fails.Count) { 1 } else { 0 })
