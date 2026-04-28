param(
    [string]$Path = "",
    [int]$DurationSec = 20,
    [int]$IntervalMs = 1000,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Continue"

if ([string]::IsNullOrWhiteSpace($Path)) {
    $Path = Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys\MouseWithoutBorders\settings.json"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputPath = Join-Path (Get-Location) ("mwb-lock-report-" + $stamp + ".txt")
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public enum RM_APP_TYPE
{
    RmUnknownApp = 0,
    RmMainWindow = 1,
    RmOtherWindow = 2,
    RmService = 3,
    RmExplorer = 4,
    RmConsole = 5,
    RmCritical = 1000
}

[StructLayout(LayoutKind.Sequential)]
public struct RM_UNIQUE_PROCESS
{
    public int dwProcessId;
    public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct RM_PROCESS_INFO
{
    public RM_UNIQUE_PROCESS Process;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
    public string strAppName;

    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
    public string strServiceShortName;

    public RM_APP_TYPE ApplicationType;
    public uint AppStatus;
    public uint TSSessionId;

    [MarshalAs(UnmanagedType.Bool)]
    public bool bRestartable;
}

public static class RestartManager
{
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmStartSession(out uint pSessionHandle, int dwSessionFlags, StringBuilder strSessionKey);

    [DllImport("rstrtmgr.dll")]
    public static extern int RmEndSession(uint pSessionHandle);

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern int RmRegisterResources(
        uint pSessionHandle,
        uint nFiles,
        string[] rgsFilenames,
        uint nApplications,
        IntPtr rgApplications,
        uint nServices,
        string[] rgsServiceNames);

    [DllImport("rstrtmgr.dll")]
    public static extern int RmGetList(
        uint dwSessionHandle,
        out uint pnProcInfoNeeded,
        ref uint pnProcInfo,
        [In, Out] RM_PROCESS_INFO[] rgAffectedApps,
        ref uint lpdwRebootReasons);
}
"@

function Get-LockingProcesses {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TargetPath
    )

    $sessionKey = New-Object System.Text.StringBuilder 64
    $sessionHandle = 0
    $startResult = [RestartManager]::RmStartSession([ref]$sessionHandle, 0, $sessionKey)
    if ($startResult -ne 0) {
        throw "RmStartSession failed with code $startResult"
    }

    try {
        $files = @($TargetPath)
        $registerResult = [RestartManager]::RmRegisterResources($sessionHandle, [uint32]$files.Length, $files, 0, [IntPtr]::Zero, 0, $null)
        if ($registerResult -ne 0) {
            throw "RmRegisterResources failed with code $registerResult"
        }

        $needed = 0
        $count = 0
        $rebootReasons = 0
        $result = [RestartManager]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $null, [ref]$rebootReasons)

        $ERROR_MORE_DATA = 234
        if ($result -eq 0 -and $needed -eq 0) {
            return @()
        }

        if ($result -ne $ERROR_MORE_DATA -and $result -ne 0) {
            throw "RmGetList probe failed with code $result"
        }

        $count = $needed
        $apps = New-Object RM_PROCESS_INFO[] $count
        $result = [RestartManager]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $apps, [ref]$rebootReasons)
        if ($result -ne 0) {
            throw "RmGetList fetch failed with code $result"
        }

        $rows = @()
        for ($i = 0; $i -lt $count; $i++) {
            $entry = $apps[$i]
            $pid = $entry.Process.dwProcessId
            $path = ""
            try {
                $proc = Get-Process -Id $pid -ErrorAction Stop
                $path = $proc.Path
            } catch {
                $path = ""
            }

            $rows += [pscustomobject]@{
                ProcessId = $pid
                AppName = $entry.strAppName
                ServiceShortName = $entry.strServiceShortName
                ApplicationType = [string]$entry.ApplicationType
                Restartable = $entry.bRestartable
                Path = $path
            }
        }

        return $rows
    } finally {
        [void][RestartManager]::RmEndSession($sessionHandle)
    }
}

$lines = New-Object System.Collections.Generic.List[string]

function Add-Line {
    param([string]$Text = "")
    $lines.Add($Text) | Out-Null
}

Add-Line "MWB settings.json lock inspector"
Add-Line ("Generated: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss zzz"))
Add-Line ("Computer: " + $env:COMPUTERNAME)
Add-Line ("User: " + $env:USERNAME)
Add-Line ("Path: " + $Path)
Add-Line ("DurationSec: " + $DurationSec)
Add-Line ("IntervalMs: " + $IntervalMs)
Add-Line ""

if (-not (Test-Path $Path)) {
    Add-Line "Target file does not exist."
} else {
    try {
        $fileInfo = Get-Item $Path -ErrorAction Stop
        Add-Line ("Exists: yes")
        Add-Line ("Length: " + $fileInfo.Length)
        Add-Line ("LastWriteTime: " + $fileInfo.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
    } catch {
        Add-Line ("Failed to stat file: " + $_.Exception.Message)
    }

    Add-Line ""
    Add-Line "=== Samples ==="

    $samples = [Math]::Max(1, [int][Math]::Ceiling(($DurationSec * 1000.0) / [Math]::Max(1, $IntervalMs)))
    for ($i = 0; $i -lt $samples; $i++) {
        $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
        Add-Line ("Time: " + $stamp)
        try {
            $lockers = @(Get-LockingProcesses -TargetPath $Path)
            if ($lockers.Count -eq 0) {
                Add-Line "Lockers: none reported by Restart Manager"
            } else {
                Add-Line ("Lockers: " + $lockers.Count)
                foreach ($locker in $lockers) {
                    Add-Line ("- pid=" + $locker.ProcessId +
                              " name=" + $locker.AppName +
                              " type=" + $locker.ApplicationType +
                              " restartable=" + $locker.Restartable +
                              $(if ($locker.ServiceShortName) { " service=" + $locker.ServiceShortName } else { "" }) +
                              $(if ($locker.Path) { " path=" + $locker.Path } else { "" }))
                }
            }
        } catch {
            Add-Line ("ERROR: " + $_.Exception.Message)
        }
        Add-Line "---"
        if ($i -lt ($samples - 1)) {
            Start-Sleep -Milliseconds $IntervalMs
        }
    }
}

[System.IO.File]::WriteAllLines($OutputPath, $lines)
Write-Host ("Report written to " + $OutputPath)
