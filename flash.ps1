<#
  flash.ps1 - quick flash of the ready-made NAV-1 firmware (no ESP-IDF, no build).

    .\flash.ps1                      # finds the board (CH340 COM port) and flashes docs\firmware\NAV1-*-full.bin
    .\flash.ps1 -Port COM5           # choose the port yourself
    .\flash.ps1 -Image my.bin        # flash another full image (merged with esptool merge_bin)

  Needs Python 3 (esptool is installed with pip if missing). Settings on the device (Wi-Fi, trips on the SD
  card) are kept: only the program is written. Alternative without any install: the web flasher in docs\index.html
  (Chrome / Edge, USB cable).
#>
param(
  [string]$Port = "",
  [string]$Image = "",
  [int]$Baud = 921600
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

if (-not $Image) {
  $Image = Get-ChildItem (Join-Path $root "docs\firmware") -Filter "NAV1-*-full.bin" | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
  if (-not $Image) { throw "no firmware image in docs\firmware" }
}
if (-not (Test-Path $Image)) { throw "image not found: $Image" }

if (-not $Port) {
  $dev = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match "CH340|CH341|USB-SERIAL|USB Serial" -and $_.Name -match "\(COM\d+\)" } | Select-Object -First 1
  if ($dev -and $dev.Name -match "\((COM\d+)\)") { $Port = $Matches[1] }
  else {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 1) { $Port = $ports[0] } else { throw "board not found - connect it with USB-C, or pass -Port COMx (ports: $($ports -join ', '))" }
  }
}

python -m esptool version *> $null
if ($LASTEXITCODE -ne 0) {
  Write-Host "installing esptool (pip)..."
  python -m pip install --quiet esptool
}

Write-Host "flashing $(Split-Path $Image -Leaf) to $Port ..."
python -m esptool --chip esp32s3 -p $Port -b $Baud --before default-reset --after hard-reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 $Image
if ($LASTEXITCODE -ne 0) {
  throw "flashing failed. Check the cable and the port; if the board does not answer: hold BOOT, tap RST, release BOOT, run again."
}
Write-Host "done - NAV-1 restarts. First GPS fix takes a few minutes outdoors."
