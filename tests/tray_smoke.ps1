param([string]$Executable = "$PSScriptRoot\..\build\Release\NvidiaQuickSwitch.exe")
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class QuickSwitchSmoke {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
}
'@
# Start only the tray; never select a preset, edit a registry entry, or invoke NVAPI.
$process = Start-Process -FilePath (Resolve-Path $Executable) -PassThru
try {
    if (-not $process.WaitForInputIdle(5000)) { throw 'Tray did not become idle.' }
    $window = [QuickSwitchSmoke]::FindWindow('NvidiaQuickSwitch.Tray', 'Nvidia Quick Switch')
    [uint32]$owner = 0
    [void][QuickSwitchSmoke]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($window -eq [IntPtr]::Zero -or $owner -ne $process.Id) { throw 'Cannot identify the test-owned tray window (another instance may already be running).' }
    $process.Refresh()
    $cpuBefore = $process.TotalProcessorTime.TotalMilliseconds
    [System.Threading.Thread]::Sleep(2000) # Fixed idle measurement window, not a completion poll.
    $process.Refresh()
    $driverModules = @($process.Modules | Where-Object { $_.ModuleName -match '^nvapi(64)?\.dll$' })
    if ($driverModules.Count) { throw 'NVAPI driver DLL was unexpectedly initialized on startup.' }
    [pscustomobject]@{
        WorkingSetMiB = [math]::Round($process.WorkingSet64 / 1MB, 2)
        PrivateBytesMiB = [math]::Round($process.PrivateMemorySize64 / 1MB, 2)
        IdleCpuMillisecondsOver2Seconds = $process.TotalProcessorTime.TotalMilliseconds - $cpuBefore
        NvapiDriverLoaded = $false
    }
} finally {
    $window = [QuickSwitchSmoke]::FindWindow('NvidiaQuickSwitch.Tray', 'Nvidia Quick Switch')
    [uint32]$owner = 0
    [void][QuickSwitchSmoke]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($window -ne [IntPtr]::Zero -and $owner -eq $process.Id) {
        [void][QuickSwitchSmoke]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $process.WaitForExit(5000)) { throw 'Test-owned tray did not exit gracefully.' }
    }
}
