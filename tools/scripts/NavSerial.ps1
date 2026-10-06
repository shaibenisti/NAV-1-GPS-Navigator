# =============================================================================
#  NavSerial.ps1 - shared serial helpers for the NAV-1 scripts (dot-source it).
#  The device console is UART0 at 115200 via the board's USB-serial bridge.
#  Opening the port does not reset the board (DTR/RTS kept low); Reset-Nav
#  pulses RTS (= EN) for a clean boot.
# =============================================================================

$script:NavPort = $null
$script:NavLog = New-Object System.Text.StringBuilder

function Open-Nav([string]$Port = "COM3", [int]$Baud = 115200) {
  $p = New-Object System.IO.Ports.SerialPort $Port, $Baud
  $p.DtrEnable = $false
  $p.RtsEnable = $false
  $p.ReadTimeout = 200
  $p.Encoding = [System.Text.Encoding]::UTF8
  $p.Open()
  $script:NavPort = $p
  [void]$script:NavLog.Clear()
}

function Close-Nav {
  if ($script:NavPort) { $script:NavPort.Close(); $script:NavPort = $null }
}

function Reset-Nav {
  $script:NavPort.RtsEnable = $true
  Start-Sleep -Milliseconds 150
  $script:NavPort.RtsEnable = $false
}

# Appends everything that arrived to the log; returns the new text.
function Receive-Nav {
  $s = ""
  try { $s = $script:NavPort.ReadExisting() } catch {}
  if ($s) { [void]$script:NavLog.Append($s) }
  return $s
}

function Wait-Nav([double]$Seconds) {
  $end = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $end) { [void](Receive-Nav); Start-Sleep -Milliseconds 50 }
}

# Reads until $Pattern (regex) appears in the text received after the call started.
# Returns the matching text window, or $null on timeout.
function Wait-NavFor([string]$Pattern, [double]$TimeoutSeconds) {
  $start = $script:NavLog.Length
  $end = (Get-Date).AddSeconds($TimeoutSeconds)
  while ((Get-Date) -lt $end) {
    [void](Receive-Nav)
    $text = $script:NavLog.ToString($start, $script:NavLog.Length - $start)
    if ($text -match $Pattern) { return $text }
    Start-Sleep -Milliseconds 50
  }
  return $null
}

function Send-Nav([string]$Line) {
  [void]$script:NavLog.Append("`n>>> $Line`n")
  $script:NavPort.Write("$Line`n")
}

# Sends a command and returns what the device printed within $Seconds.
function Invoke-Nav([string]$Line, [double]$Seconds = 1.0) {
  Send-Nav $Line
  $start = $script:NavLog.Length
  Wait-Nav $Seconds
  return $script:NavLog.ToString($start, $script:NavLog.Length - $start)
}

function Get-NavLog { return $script:NavLog.ToString() }

# Copies bytes to a file on the SD card ("sd put": base64 lines, ack every 32 lines). ~8 KB/s.
# Throws on failure.
function Send-NavFile([byte[]]$Bytes, [string]$Remote) {
  $chunk = 72                                     # 72 bytes -> 96 base64 chars (console lines < 128)
  $lines = [Math]::Ceiling($Bytes.Length / $chunk)
  Send-Nav "sd put $Remote $($Bytes.Length)"
  $r = Wait-NavFor "\[PUT\] (ready|FAILED)[^\n]*\n" 5
  if (-not $r -or $r -match "FAILED") { throw "sd put refused: $r" }
  $sb = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $lines; $i++) {
    $n = [Math]::Min($chunk, $Bytes.Length - $i * $chunk)
    [void]$sb.Append([Convert]::ToBase64String($Bytes, $i * $chunk, $n)).Append("`n")
    if ((($i + 1) % 32) -eq 0) {                  # block of 32 lines, then wait for the device's ack
      $script:NavPort.Write($sb.ToString()); [void]$sb.Clear()
      if (-not (Wait-NavFor "\[PUT\] $([Math]::Min($Bytes.Length, ($i + 1) * $chunk))\r?\n" 10)) { throw "no ack after line $($i + 1)" }
      Write-Progress -Activity "sd put $Remote" -PercentComplete (100 * ($i + 1) / $lines)
    }
  }
  $script:NavPort.Write($sb.ToString() + ".`n")
  $r = Wait-NavFor "\[PUT\] (ok|FAILED)[^\n]*\n" 30
  Write-Progress -Activity "sd put $Remote" -Completed
  if (-not $r -or $r -notmatch "\[PUT\] ok") { throw "sd put failed: $r" }
}
