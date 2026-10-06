<#
  sdput.ps1 - copy a file from the PC to the NAV-1 SD card over the console (no card removal).

  ~8 KB/s at 115200 baud: fine for settings, icons and small images. For big files
  (a full-screen wallpaper is 768 KB, ~1.5 min) copying with a card reader is faster.

  Examples:
    .\tools\scripts\sdput.ps1 my-settings.json /system/settings.json
    .\tools\scripts\sdput.ps1 icons\GPS.bin /assets/icons/GPS.bin
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory, Position = 0)][string]$Local,
  [Parameter(Mandatory, Position = 1)][string]$Remote,
  [string]$Port = "COM3"
)
. "$PSScriptRoot\NavSerial.ps1"

$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Local))
$sw = [Diagnostics.Stopwatch]::StartNew()
Open-Nav $Port
try {
  Send-NavFile $bytes $Remote
  Write-Host ("{0} -> {1}: {2} bytes in {3:N1} s" -f $Local, $Remote, $bytes.Length, $sw.Elapsed.TotalSeconds)
} finally { Close-Nav }
