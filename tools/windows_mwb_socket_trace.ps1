param(
    [int]$HoursBack = 2,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Continue"

function Add-Line {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Text = ""
    )
    $Lines.Add($Text) | Out-Null
}

function Add-Section {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Title
    )
    Add-Line $Lines ""
    Add-Line $Lines ("=== " + $Title + " ===")
}

function Safe-String {
    param($Value)
    if ($null -eq $Value) {
        return ""
    }
    return [string]$Value
}

function Trim-Message {
    param(
        [string]$Message,
        [int]$MaxLines = 18,
        [int]$MaxChars = 3200
    )

    if ([string]::IsNullOrWhiteSpace($Message)) {
        return ""
    }

    $normalized = $Message -replace "`r", ""
    $parts = $normalized -split "`n"
    if ($parts.Count -gt $MaxLines) {
        $parts = $parts[0..($MaxLines - 1)]
    }

    $text = ($parts -join "`n").Trim()
    if ($text.Length -gt $MaxChars) {
        $text = $text.Substring(0, $MaxChars) + "... [truncated]"
    }
    return $text
}

function Add-CommandOutput {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Label,
        [scriptblock]$Command
    )

    Add-Line $Lines ("-- " + $Label)
    try {
        $output = & $Command 2>&1
        if ($null -eq $output) {
            Add-Line $Lines "(no output)"
            return
        }

        foreach ($item in @($output)) {
            if ($item -is [string]) {
                Add-Line $Lines $item
                continue
            }

            if ($item -is [System.Management.Automation.ErrorRecord]) {
                Add-Line $Lines ($item.ToString())
                continue
            }

            $rendered = ($item | Out-String -Width 260).TrimEnd()
            if ([string]::IsNullOrWhiteSpace($rendered)) {
                Add-Line $Lines (Safe-String $item)
            } else {
                foreach ($line in ($rendered -split "`r?`n")) {
                    Add-Line $Lines $line
                }
            }
        }
    } catch {
        Add-Line $Lines ("ERROR: " + $_.Exception.Message)
    }
}

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        return $null
    }
    try {
        return Get-Content -Path $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
    } catch {
        return $null
    }
}

function Add-SettingsSnapshot {
    param(
        [System.Collections.Generic.List[string]]$Lines
    )

    Add-Section $Lines "Mouse Without Borders Settings"

    $settingsPath = Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys\MouseWithoutBorders\settings.json"
    Add-Line $Lines ("Path: " + $settingsPath)

    $json = Read-JsonFile -Path $settingsPath
    if ($null -eq $json) {
        Add-Line $Lines "Unable to parse settings.json"
        return
    }

    $props = $json.properties
    Add-Line $Lines ("SecurityKey: " + (Safe-String $props.SecurityKey.value))
    Add-Line $Lines ("UseService: " + (Safe-String $props.UseService.value))
    Add-Line $Lines ("MachineMatrixString: " + (($props.MachineMatrixString | ForEach-Object { Safe-String $_ }) -join "," ))
    Add-Line $Lines ("MachinePool: " + (Safe-String $props.MachinePool.value))
    Add-Line $Lines ("Name2IP: " + (Safe-String $props.Name2IP.value))
    Add-Line $Lines ("MachineID: " + (Safe-String $props.MachineID.value))
    Add-Line $Lines ("TCPPort: " + (Safe-String $props.TCPPort.value))
    Add-Line $Lines ("ValidateRemoteMachineIP: " + (Safe-String $props.ValidateRemoteMachineIP.value))
    Add-Line $Lines ("SameSubnetOnly: " + (Safe-String $props.SameSubnetOnly.value))
}

function Get-KnownLogRoots {
    $roots = @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys\MouseWithoutBorders"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys"),
        (Join-Path $env:LOCALAPPDATA "PowerToys"),
        (Join-Path $env:PROGRAMDATA "PowerToys")
    )

    return $roots | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
}

function Add-ProcessSnapshot {
    param(
        [System.Collections.Generic.List[string]]$Lines
    )

    Add-Section $Lines "PowerToys / MWB Processes"

    Add-CommandOutput $Lines "Get-Process" {
        Get-Process -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProcessName -match "PowerToys|MouseWithoutBorders|Borders"
            } |
            Select-Object ProcessName, Id, StartTime, CPU, Path |
            Sort-Object ProcessName, Id
    }

    Add-CommandOutput $Lines "Win32_Process details" {
        Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name -match "PowerToys|MouseWithoutBorders|Borders" -or
                $_.ExecutablePath -match "PowerToys|MouseWithoutBorders|Borders" -or
                $_.CommandLine -match "PowerToys|MouseWithoutBorders|Borders"
            } |
            Select-Object Name, ProcessId, ParentProcessId, ExecutablePath, CommandLine |
            Sort-Object Name, ProcessId
    }
}

function Add-ServiceSnapshot {
    param(
        [System.Collections.Generic.List[string]]$Lines
    )

    Add-Section $Lines "PowerToys / MWB Services"

    Add-CommandOutput $Lines "Get-Service" {
        Get-Service -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name -match "PowerToys|Mouse|Borders" -or
                $_.DisplayName -match "PowerToys|Mouse|Borders"
            } |
            Select-Object Name, DisplayName, Status, StartType
    }

    Add-CommandOutput $Lines "Win32_Service details" {
        Get-CimInstance Win32_Service -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name -match "PowerToys|Mouse|Borders" -or
                $_.DisplayName -match "PowerToys|Mouse|Borders" -or
                $_.PathName -match "PowerToys|MouseWithoutBorders|Borders"
            } |
            Select-Object Name, DisplayName, State, StartMode, ProcessId, PathName |
            Sort-Object Name
    }
}

function Add-RecentFileInventory {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [datetime]$StartTime
    )

    Add-Section $Lines "Recent PowerToys / MWB Files"

    $matchedAnyRoot = $false
    foreach ($root in (Get-KnownLogRoots)) {
        if (-not (Test-Path $root)) {
            continue
        }

        $matchedAnyRoot = $true
        Add-Line $Lines ("Root: " + $root)

        try {
            $files = Get-ChildItem -Path $root -Recurse -File -ErrorAction Stop |
                Where-Object {
                    $_.LastWriteTime -ge $StartTime -or
                    $_.Name -match "MouseWithoutBorders|PowerToys|service|helper|log|trace|txt|json"
                } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 20

            if (-not $files) {
                Add-Line $Lines "No recent or MWB-related files found."
                Add-Line $Lines "---"
                continue
            }

            foreach ($file in $files) {
                Add-Line $Lines ("File: " + $file.FullName)
                Add-Line $Lines ("Modified: " + $file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
                Add-Line $Lines ("Size: " + $file.Length)
                Add-Line $Lines "---"
            }
        } catch {
            Add-Line $Lines ("ERROR scanning files: " + $_.Exception.Message)
            Add-Line $Lines "---"
        }
    }

    if (-not $matchedAnyRoot) {
        Add-Line $Lines "No known PowerToys roots found."
    }
}

function Add-EventSummary {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [datetime]$StartTime
    )

    Add-Section $Lines "Recent Event Log Matches"

    Add-CommandOutput $Lines "Application events" {
        Get-WinEvent -FilterHashtable @{
            LogName = "Application"
            StartTime = $StartTime
        } -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProviderName -in @("Application Error", ".NET Runtime", "Windows Error Reporting") -or
                $_.Message -match "PowerToys|Mouse Without Borders|MouseWithoutBorders|Borders"
            } |
            Select-Object -First 25 TimeCreated, ProviderName, Id, LevelDisplayName,
                @{ Name = "Message"; Expression = { Trim-Message -Message (Safe-String $_.Message) } }
    }

    Add-CommandOutput $Lines "System events" {
        Get-WinEvent -FilterHashtable @{
            LogName = "System"
            StartTime = $StartTime
        } -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProviderName -in @("Service Control Manager") -or
                $_.Message -match "PowerToys|Mouse Without Borders|MouseWithoutBorders|Borders"
            } |
            Select-Object -First 25 TimeCreated, ProviderName, Id, LevelDisplayName,
                @{ Name = "Message"; Expression = { Trim-Message -Message (Safe-String $_.Message) } }
    }
}

function Add-FilteredLogs {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [datetime]$StartTime
    )

    Add-Section $Lines "Filtered PowerToys Logs"

    $patterns = @(
        "MouseWithoutBorders",
        "Machine got trusted",
        "Connected to new machine",
        "Invalid ACK",
        "Invalid package",
        "TcpReceive error",
        "Duplicate connected client socket",
        "Connected to/from local socket",
        "ForceClosed",
        "InvalidKey",
        "UpdateTcpSockets",
        "MainTCPRoutine",
        "MainUdpRoutine",
        "Heartbeat_ex",
        "Heartbeat_ex_l2",
        "Heartbeat_ex_l3",
        "Hello",
        "Awake",
        "PowerToys.MouseWithoutBorders",
        "MouseWithoutBordersService",
        "MouseWithoutBordersHelper",
        "settings.json",
        "Failed to read settings"
    )

    $matchedAnyRoot = $false

    foreach ($root in (Get-KnownLogRoots)) {
        if (-not (Test-Path $root)) {
            continue
        }

        $matchedAnyRoot = $true
        Add-Line $Lines ("Root: " + $root)

        $files = Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                ($_.LastWriteTime -ge $StartTime) -and
                (
                    $_.Extension -in @(".log", ".txt", ".json") -or
                    $_.Name -match "MouseWithoutBorders|PowerToys|service|helper|log|trace"
                )
            } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 25

        if (-not $files) {
            Add-Line $Lines "No recent log files found."
            Add-Line $Lines "---"
            continue
        }

        foreach ($file in $files) {
            Add-Line $Lines ("File: " + $file.FullName)
            Add-Line $Lines ("Modified: " + $file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))

            try {
                $linesOut = Select-String -Path $file.FullName -Pattern $patterns -SimpleMatch -ErrorAction Stop |
                    Select-Object -Last 120

                if (-not $linesOut) {
                    Add-Line $Lines "(no matching lines)"
                } else {
                    foreach ($match in $linesOut) {
                        Add-Line $Lines ($match.LineNumber.ToString().PadLeft(6) + ": " + (Safe-String $match.Line))
                    }
                }
            } catch {
                Add-Line $Lines ("ERROR reading file: " + $_.Exception.Message)
            }

            Add-Line $Lines "---"
        }
    }

    if (-not $matchedAnyRoot) {
        Add-Line $Lines "No known PowerToys log roots found."
    }
}

$now = Get-Date
$startTime = $now.AddHours(-1 * [math]::Abs($HoursBack))

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Get-Location) ("mwb-socket-trace-" + $now.ToString("yyyyMMdd-HHmmss") + ".txt")
}

$lines = New-Object 'System.Collections.Generic.List[string]'
Add-Line $lines "Mouse Without Borders targeted socket trace"
Add-Line $lines ("Generated: " + $now.ToString("yyyy-MM-dd HH:mm:ss zzz"))
Add-Line $lines ("Computer: " + $env:COMPUTERNAME)
Add-Line $lines ("User: " + $env:USERNAME)
Add-Line $lines ("HoursBack: " + $HoursBack)

Add-SettingsSnapshot -Lines $lines
Add-ProcessSnapshot -Lines $lines
Add-ServiceSnapshot -Lines $lines
Add-RecentFileInventory -Lines $lines -StartTime $startTime
Add-EventSummary -Lines $lines -StartTime $startTime
Add-FilteredLogs -Lines $lines -StartTime $startTime

try {
    Set-Content -Path $OutputPath -Value $lines -Encoding UTF8
    Write-Host ("Wrote report: " + $OutputPath)
} catch {
    Write-Host ("Failed to write report: " + $_.Exception.Message)
    exit 1
}
