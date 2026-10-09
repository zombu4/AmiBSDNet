# Drive the WinUAE window: click / double-click at screenshot coordinates,
# type text, press keys, capture the window.  Needs WinUAE's magic mouse
# (absolute_mouse=mousehack) so the Amiga pointer follows the host cursor.
#
#   uaeinput.ps1 shot  <out.png>
#   uaeinput.ps1 click <x> <y>        (coordinates in the captured image)
#   uaeinput.ps1 dclick <x> <y>
#   uaeinput.ps1 rclick <x> <y>       (right button: hold for menus)
#   uaeinput.ps1 type  "<text>"       (SendKeys syntax: {ENTER} {ESC} ...)
param([string]$Cmd, [string]$A1, [string]$A2)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class U {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
[U]::SetProcessDPIAware() | Out-Null
$p = Get-Process winuae64 -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "WinUAE window not found"; exit 1 }
$h = $p.MainWindowHandle

function Focus {
  # Never send input unless WinUAE really is in front: it would land in the
  # user's window otherwise.  Only if it is not, use the synthetic Alt tap
  # Windows requires before allowing a focus change (it reaches the Amiga
  # as an Alt key press, so avoid it when not needed).
  if ([U]::GetForegroundWindow() -ne $h) {
    [U]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)
    [U]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
    [U]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300
  }
  if ([U]::GetForegroundWindow() -ne $h) {
    Write-Error "WinUAE is not the foreground window: input NOT sent"
    exit 2
  }
}
function MoveTo([int]$x, [int]$y) {
  # move like a real mouse, in steps, so WinUAE's magic mouse sees motion
  # and the Amiga pointer is really there before any button is pressed
  $pt = New-Object U+POINT; $pt.X = $x; $pt.Y = $y
  [U]::ClientToScreen($h, [ref]$pt) | Out-Null
  $cur = [System.Windows.Forms.Cursor]::Position
  for ($i = 1; $i -le 12; $i++) {
    [U]::SetCursorPos([int]($cur.X + ($pt.X - $cur.X) * $i / 12),
                      [int]($cur.Y + ($pt.Y - $cur.Y) * $i / 12)) | Out-Null
    Start-Sleep -Milliseconds 25
  }
  Start-Sleep -Milliseconds 350
}
function Click([uint32]$down, [uint32]$up) {
  [U]::mouse_event($down, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
  [U]::mouse_event($up, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
}

switch ($Cmd) {
  "shot" {
    $r = New-Object U+RECT; [U]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [U]::PrintWindow($h, $dc, 3) | Out-Null; $g.ReleaseHdc($dc)
    $bmp.Save($A1, [System.Drawing.Imaging.ImageFormat]::Png); "saved $A1"
  }
  "click"  { Focus; MoveTo $A1 $A2; Click 0x2 0x4; "clicked $A1,$A2" }
  "dclick" { Focus; MoveTo $A1 $A2; Click 0x2 0x4; Click 0x2 0x4; "double-clicked $A1,$A2" }
  "rclick" { Focus; MoveTo $A1 $A2; Click 0x8 0x10; "right-clicked $A1,$A2" }
  "drag"   {
    # drag x1,y1 -> x2,y2 with the left button: args "x1,y1" "x2,y2"
    $f = $A1.Split(","); $t = $A2.Split(",")
    Focus; MoveTo $f[0] $f[1]
    [U]::mouse_event(0x2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 150
    for ($i = 1; $i -le 10; $i++) {
      MoveTo ([int]$f[0] + ([int]$t[0] - [int]$f[0]) * $i / 10) ([int]$f[1] + ([int]$t[1] - [int]$f[1]) * $i / 10)
    }
    [U]::mouse_event(0x4, 0, 0, 0, [IntPtr]::Zero); "dragged $A1 -> $A2"
  }
  "type"   { Focus; [System.Windows.Forms.SendKeys]::SendWait($A1); "typed" }
  default  { Write-Error "unknown command $Cmd"; exit 1 }
}
