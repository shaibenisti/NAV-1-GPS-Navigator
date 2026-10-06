<#
  imgconv.ps1 - convert a PNG/JPG into an LVGL 9 binary image for NAV-1 (Assets).

  Output: 12-byte LVGL header + pixels. RGB565 (opaque) or RGB565A8 (with transparency,
  chosen automatically when the picture has any). Copy the result to the SD card:
    icons       /assets/icons/<App name>.bin      (<= 128 x 128, e.g. GPS.bin, Trips.bin)
    wallpaper   /assets/wallpapers/home.bin       (<= 800 x 480, centered)
  With a card reader, or over the console: .\tools\scripts\sdput.ps1 <file> <path>

  Examples:
    .\tools\scripts\imgconv.ps1 gps.png GPS.bin -Size 96
    .\tools\scripts\imgconv.ps1 photo.jpg home.bin -Width 800 -Height 480
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory, Position = 0)][string]$In,
  [Parameter(Mandatory, Position = 1)][string]$Out,
  [int]$Size = 0,                       # square icon size (resizes)
  [int]$Width = 0, [int]$Height = 0,    # or explicit size (resizes)
  [ValidateSet("auto", "RGB565", "RGB565A8")][string]$Format = "auto"
)
Add-Type -AssemblyName System.Drawing
if (-not ("NavImg" -as [type])) {
  Add-Type -TypeDefinition @"
public static class NavImg {
  // px: 32 bpp BGRA rows (srcStride bytes). LVGL 9: magic 0x19, RGB565 0x12, RGB565A8 0x14
  // (checked by static_assert in Assets.cpp).
  public static byte[] Convert(byte[] px, int srcStride, int w, int h, string format) {
    bool alpha = false;
    for (int i = 3; i < px.Length; i += 4) if (px[i] != 255) { alpha = true; break; }
    bool a8 = format == "RGB565A8" || (format == "auto" && alpha);
    int stride = w * 2, color = stride * h, size = 12 + color + (a8 ? w * h : 0);
    var o = new byte[size];
    o[0] = 0x19; o[1] = (byte)(a8 ? 0x14 : 0x12);
    o[4] = (byte)w; o[5] = (byte)(w >> 8); o[6] = (byte)h; o[7] = (byte)(h >> 8);
    o[8] = (byte)stride; o[9] = (byte)(stride >> 8);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int s = y * srcStride + x * 4;              // BGRA
        int b = px[s], gg = px[s + 1], r = px[s + 2], a = px[s + 3];
        if (!a8 && a < 255) { r = r * a / 255; gg = gg * a / 255; b = b * a / 255; }   // flatten on black
        int v = ((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3);
        int t = 12 + y * stride + x * 2;
        o[t] = (byte)v; o[t + 1] = (byte)(v >> 8);
        if (a8) o[12 + color + y * w + x] = (byte)a;
      }
    return o;
  }
}
"@
}
$src = [System.Drawing.Image]::FromFile((Resolve-Path $In))
try {
  $w = $src.Width; $h = $src.Height
  if ($Size) { $w = $Size; $h = $Size } elseif ($Width -and $Height) { $w = $Width; $h = $Height }
  $bmp = New-Object System.Drawing.Bitmap $src, $w, $h           # resized, 32 bpp ARGB
  $d = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                     [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $px = New-Object byte[] ($d.Stride * $h)
  [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $px, 0, $px.Length)
  $bmp.UnlockBits($d); $bmp.Dispose()
  $bytes = [NavImg]::Convert($px, $d.Stride, $w, $h, $Format)
  [IO.File]::WriteAllBytes($(if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }), $bytes)
  Write-Host ("{0} -> {1}: {2} x {3} {4}, {5} bytes" -f $In, $Out, $w, $h, $(if ($bytes[1] -eq 0x14) { "RGB565A8" } else { "RGB565" }), $bytes.Length)
} finally { $src.Dispose() }
