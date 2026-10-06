<#
  nav.ps1 - send console commands to the running NAV-1 and print the replies.

  Examples:
    .\tools\scripts\nav.ps1 help
    .\tools\scripts\nav.ps1 "diag gps" "diag mem"
    .\tools\scripts\nav.ps1 "wifi connect ""MyNet"" secret" -Wait 15
    .\tools\scripts\nav.ps1 selftest -Wait 40
    .\tools\scripts\nav.ps1 -Reset -Wait 15          # reboot and show the boot log
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Commands = @(),
  [string]$Port = "COM3",
  [double]$Wait = 2,
  [switch]$Reset
)
. "$PSScriptRoot\NavSerial.ps1"

Open-Nav $Port
try {
  if ($Reset) { Reset-Nav; Wait-Nav $Wait; Write-Host (Get-NavLog) }
  foreach ($c in $Commands) { Write-Host (Invoke-Nav $c $Wait) }
} finally { Close-Nav }
