<#
  shot.ps1 - screenshot of the NAV-1 display (the framebuffer being shown) as PNG.

  Examples:
    .\tools\scripts\shot.ps1                       # current screen -> tools\scripts\shots\<time>.png
    .\tools\scripts\shot.ps1 -Open Files -Name files   # open an app first, then shoot
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [string]$Port = "COM3",
  [string]$Open = "",                  # console command(s) before the shot, e.g. "open Files"; ";"-separated
  [string]$Name = "",
  [double]$Settle = 1.5
)
. "$PSScriptRoot\NavSerial.ps1"
Add-Type -AssemblyName System.Drawing
$dir = Join-Path $PSScriptRoot "shots"
New-Item -ItemType Directory -Force $dir | Out-Null
if (-not $Name) { $Name = Get-Date -Format "yyyyMMdd-HHmmss" }

Open-Nav $Port
try {
  foreach ($c in ($Open -split ";" | Where-Object { $_.Trim() })) { [void](Invoke-Nav $c.Trim() $Settle) }
  Send-Nav "shot"
  $t = Wait-NavFor "(?s)<<<SHOT \d+ \d+ \d+>>>.*?<<<END SHOT>>>" 120
  if (-not $t) { throw "no screenshot received" }
} finally { Close-Nav }

$m = [regex]::Match($t, "(?s)<<<SHOT (\d+) (\d+) (\d+)>>>\r?\n(.*?)<<<END SHOT>>>")
$w = [int]$m.Groups[1].Value; $h = [int]$m.Groups[2].Value
$rle = [Convert]::FromBase64String(($m.Groups[4].Value -replace "\s", ""))
if ($rle.Length -ne [int]$m.Groups[3].Value) { throw "size mismatch: got $($rle.Length) of $($m.Groups[3].Value) bytes" }
if (-not ("NavShot" -as [type])) {
  Add-Type -TypeDefinition @"
public static class NavShot {
  public static byte[] Decode(byte[] rle, int n) {       // -> 32 bpp BGRA
    var o = new byte[n * 4]; int p = 0;
    for (int i = 0; i + 3 < rle.Length && p < n; i += 4) {
      int run = rle[i] | (rle[i + 1] << 8), v = rle[i + 2] | (rle[i + 3] << 8);
      byte r = (byte)(((v >> 11) & 31) * 255 / 31), g = (byte)(((v >> 5) & 63) * 255 / 63), b = (byte)((v & 31) * 255 / 31);
      for (int k = 0; k < run && p < n; k++, p++) { o[p * 4] = b; o[p * 4 + 1] = g; o[p * 4 + 2] = r; o[p * 4 + 3] = 255; }
    }
    return o;
  }
}
"@
}
$px = [NavShot]::Decode($rle, $w * $h)
$bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$d = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
[Runtime.InteropServices.Marshal]::Copy($px, 0, $d.Scan0, $px.Length)
$bmp.UnlockBits($d)
if ($w -gt $h) { $bmp.RotateFlip([System.Drawing.RotateFlipType]::Rotate270FlipNone) }   # portrait UI (LCD_PORTRAIT): the panel scans landscape
$file = Join-Path $dir "$Name.png"
$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Host "$file ($($rle.Length) bytes over serial)"
