# Capture only the WinUAE window's own contents (PrintWindow), even when it is
# covered by other windows.  Never captures the rest of the desktop.
param([string]$Out = "build\winuae.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class PW {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int L, T, R, B; }
}
"@
[PW]::SetProcessDPIAware() | Out-Null
$p = Get-Process winuae64 -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "WinUAE window not found"; exit 1 }
$r = New-Object PW+RECT
[PW]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = [Math]::Max(1, $r.R - $r.L); $h = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 1 = PW_CLIENTONLY, 2 = PW_RENDERFULLCONTENT (needed for Direct3D output)
[PW]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ${w}x${h}"
