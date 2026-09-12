param(
    [string]$Exe,
    [string]$Config,
    [string]$OutputDirectory,
    [switch]$Baseline,
    [switch]$Media
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TimelineInput {
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr window, ref POINT point);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
}
'@
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$process = Start-Process -FilePath $Exe -ArgumentList @('--automation', ('"' + $Config + '"')) -WindowStyle Hidden -PassThru
$ae = [System.Windows.Automation.AutomationElement]
$condition = New-Object System.Windows.Automation.PropertyCondition($ae::ProcessIdProperty, $process.Id)
function Windows { $ae::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $condition) }
function Bounds($window) {
    $r = New-Object TimelineInput+RECT
    [TimelineInput]::GetWindowRect([IntPtr]$window.Current.NativeWindowHandle, [ref]$r) | Out-Null
    New-Object System.Drawing.Rectangle($r.left, $r.top, ($r.right - $r.left), ($r.bottom - $r.top))
}
function Shot($window, $name) {
    $r = Bounds $window
    $bmp = New-Object System.Drawing.Bitmap([int]$r.Width, [int]$r.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    try { $graphics.CopyFromScreen([int]$r.X, [int]$r.Y, 0, 0, $bmp.Size) }
    catch {
        $dc = $graphics.GetHdc()
        try { if (![TimelineInput]::PrintWindow([IntPtr]$window.Current.NativeWindowHandle, $dc, 2)) { throw 'PrintWindow failed' } }
        finally { $graphics.ReleaseHdc($dc) }
        'Screenshot method: PrintWindow (desktop DC unavailable)' | Add-Content (Join-Path $OutputDirectory 'capture-method.txt')
    }
    $bmp.Save((Join-Path $OutputDirectory $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bmp.Dispose()
}
function MouseMessage($message, $flags) { if ($script:mainHandle) { [TimelineInput]::SendMessage($script:mainHandle, $message, [IntPtr]$flags, [IntPtr]$script:mousePoint) | Out-Null } }
function Point($x, $y) {
    $point = New-Object TimelineInput+POINT; $point.x = [int]$x; $point.y = [int]$y
    [TimelineInput]::ScreenToClient($script:mainHandle, [ref]$point) | Out-Null
    $script:mousePoint = ($point.y -shl 16) -bor ($point.x -band 65535)
    MouseMessage 0x200 $script:mouseFlags
}
function Down { $script:mouseFlags = 1; MouseMessage 0x201 1 }
function Up { $script:mouseFlags = 0; MouseMessage 0x202 0 }
function Key($code) {
    [TimelineInput]::SendMessage($script:mainHandle, 0x100, [IntPtr]$code, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 80
    [TimelineInput]::SendMessage($script:mainHandle, 0x101, [IntPtr]$code, [IntPtr]0xC0000001L) | Out-Null
}
function Button($window, $escaped) {
    $name = [regex]::Unescape($escaped)
    $items = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $button = $items | Where-Object { $_.Current.Name -eq $name -and $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button } | Select-Object -First 1
    if (!$button) { throw "Missing button: $escaped" }
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
try {
    $window = $null
    for ($i = 0; $i -lt 40; ++$i) {
        Start-Sleep -Milliseconds 250
        if ($process.HasExited) { throw "Validation window exited: $($process.ExitCode)" }
        $window = Windows | Sort-Object { $_.Current.BoundingRectangle.Width } -Descending | Select-Object -First 1
        if ($window -and $window.Current.BoundingRectangle.Width -gt 300) { break }
    }
    if (!$window) { throw 'Validation window did not appear' }
    $script:mainHandle = [IntPtr]$window.Current.NativeWindowHandle; $script:mousePoint = 0; $script:mouseFlags = 0
    [TimelineInput]::ShowWindow($script:mainHandle, 5) | Out-Null
    [TimelineInput]::SetForegroundWindow([IntPtr]$window.Current.NativeWindowHandle) | Out-Null
    Start-Sleep -Seconds 2
    $r = Bounds $window
    Shot $window '01-initial.png'
    $items = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $items | ForEach-Object { "$($_.Current.ControlType.ProgrammaticName) $($_.Current.Name) $($_.Current.BoundingRectangle)" } | Set-Content -Encoding UTF8 (Join-Path $OutputDirectory 'controls.txt')
    if ($Baseline) {
        Point ($r.X + 488) ($r.Y + 123); Down; Start-Sleep -Milliseconds 150; Up
        Start-Sleep -Milliseconds 500; Shot $window '02-scrub-release.png'
        Point ($r.X + 510) ($r.Y + 172); Down; Point ($r.X + 513) ($r.Y + 172)
        Start-Sleep -Milliseconds 400; Shot $window '03-three-pixel-drag.png'; Up
    } elseif ($Media) {
        Button $window '\uC7AC\uC0DD'
        Start-Sleep -Seconds 4; Shot $window '02-media-playing.png'
        Button $window '\uC77C\uC2DC\uC815\uC9C0'
        Start-Sleep -Seconds 1; Shot $window '03-media-paused.png'
    } else {
        Point ($r.X + 480) ($r.Y + 567); Down; Point ($r.X + 540) ($r.Y + 567)
        Start-Sleep -Milliseconds 400; Shot $window '01b-drag-ghost.png'
        Key 0x1B; Up; Start-Sleep -Milliseconds 250; Shot $window '01c-escape-cancel.png'
        Button $window '\uB179\uD654 \uC2DC\uC791'
        Start-Sleep -Seconds 3; Shot $window '02-recording-live-a.png'
        Start-Sleep -Seconds 2; Shot $window '03-recording-live-b.png'
        Button $window '\uC815\uC9C0'
        Start-Sleep -Milliseconds 400; Shot $window '04-stopped.png'
        Button $window '\uC124\uC815'
        Start-Sleep -Milliseconds 500
        $dialog = Windows | Where-Object { $_.Current.Name -match ([regex]::Unescape('\uB2E8\uCD95\uD0A4')) } | Select-Object -First 1
        if ($dialog) { Shot $dialog '05-shortcuts.png'; $dialog.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() }
    }
    $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    $process.WaitForExit(10000) | Out-Null
    "validation PID=$($process.Id) exit=$($process.ExitCode)"
} finally {
    Up
    if (!$process.HasExited) { $process.CloseMainWindow() | Out-Null; $process.WaitForExit(5000) | Out-Null }
}
