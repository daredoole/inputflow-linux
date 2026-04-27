param(
    [string]$LinuxIp = "",
    [int]$Port = 15101,
    [int]$HoursBack = 6,
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
        [int]$MaxLines = 14,
        [int]$MaxChars = 2400
    )

    if ([string]::IsNullOrEmpty($Message)) {
        return ""
    }

    $normalized = $Message -replace "`r", ""
    $lines = $normalized -split "`n"
    if ($lines.Count -gt $MaxLines) {
        $lines = $lines[0..($MaxLines - 1)]
    }

    $text = ($lines -join "`n").Trim()
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

            $rendered = ($item | Out-String -Width 240).TrimEnd()
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

function Add-EventSummary {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [datetime]$StartTime
    )

    Add-Section $Lines "Recent Application Events"

    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName = "Application"
            StartTime = $StartTime
        } -ErrorAction Stop |
        Where-Object {
            $_.ProviderName -in @("Application Error", ".NET Runtime", "Windows Error Reporting") -or
            $_.Message -match "PowerToys|Mouse Without Borders|MouseWithoutBorders|PowerLauncher"
        } |
        Select-Object -First 20

        if (-not $events) {
            Add-Line $Lines "No matching Application log events found."
            return
        }

        foreach ($event in $events) {
            Add-Line $Lines ("Time: " + $event.TimeCreated.ToString("yyyy-MM-dd HH:mm:ss"))
            Add-Line $Lines ("Provider: " + $event.ProviderName + "  Id: " + $event.Id + "  Level: " + $event.LevelDisplayName)
            $message = Trim-Message -Message (Safe-String $event.Message)
            if ($message) {
                Add-Line $Lines $message
            }
            Add-Line $Lines "---"
        }
    } catch {
        Add-Line $Lines ("ERROR: " + $_.Exception.Message)
    }
}

function Add-PowerToysLogs {
    param(
        [System.Collections.Generic.List[string]]$Lines
    )

    Add-Section $Lines "PowerToys Log Snippets"

    $roots = @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys"),
        (Join-Path $env:LOCALAPPDATA "PowerToys"),
        (Join-Path $env:PROGRAMDATA "PowerToys")
    ) | Select-Object -Unique

    $found = $false

    foreach ($root in $roots) {
        if (-not (Test-Path $root)) {
            continue
        }

        $found = $true
        Add-Line $Lines ("Root: " + $root)

        try {
            $files = Get-ChildItem -Path $root -Recurse -File -ErrorAction Stop |
                Where-Object {
                    $_.Extension -in @(".log", ".txt") -or
                    $_.Name -match "log|trace|crash|error|MouseWithoutBorders|PowerToys"
                } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 5

            if (-not $files) {
                Add-Line $Lines "No log-like files found under this root."
                Add-Line $Lines "---"
                continue
            }

            foreach ($file in $files) {
                Add-Line $Lines ("File: " + $file.FullName)
                Add-Line $Lines ("Modified: " + $file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
                try {
                    $tail = Get-Content -Path $file.FullName -Tail 60 -ErrorAction Stop
                    if ($tail) {
                        foreach ($line in $tail) {
                            Add-Line $Lines (Safe-String $line)
                        }
                    } else {
                        Add-Line $Lines "(empty file)"
                    }
                } catch {
                    Add-Line $Lines ("ERROR reading file: " + $_.Exception.Message)
                }
                Add-Line $Lines "---"
            }
        } catch {
            Add-Line $Lines ("ERROR scanning logs: " + $_.Exception.Message)
            Add-Line $Lines "---"
        }
    }

    if (-not $found) {
        Add-Line $Lines "No known PowerToys log roots found."
    }
}

$now = Get-Date
$startTime = $now.AddHours(-1 * [math]::Abs($HoursBack))
$linuxClipboardPort = $Port - 1

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Get-Location) ("mwb-windows-report-" + $now.ToString("yyyyMMdd-HHmmss") + ".txt")
}

$lines = New-Object 'System.Collections.Generic.List[string]'

Add-Line $lines "Mouse Without Borders / PowerToys Windows collector"
Add-Line $lines ("Generated: " + $now.ToString("yyyy-MM-dd HH:mm:ss zzz"))
Add-Line $lines ("Computer: " + $env:COMPUTERNAME)
Add-Line $lines ("User: " + $env:USERNAME)
Add-Line $lines ("LinuxIp: " + $(if ($LinuxIp) { $LinuxIp } else { "<not supplied>" }))
Add-Line $lines ("Port: " + $Port)
Add-Line $lines ("HoursBack: " + $HoursBack)

Add-Section $lines "OS"
Add-CommandOutput $lines "Computer info" {
    Get-ComputerInfo |
        Select-Object WindowsProductName, WindowsVersion, OsName, OsVersion, OsBuildNumber, CsDNSHostName |
        Format-List |
        Out-String -Width 240
}

Add-Section $lines "Network"
Add-CommandOutput $lines "IPv4 addresses" {
    Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notmatch '^127\.' } |
        Select-Object InterfaceAlias, IPAddress, PrefixLength |
        Format-Table -AutoSize |
        Out-String -Width 240
}

Add-CommandOutput $lines "Net adapters" {
    Get-NetAdapter |
        Select-Object Name, InterfaceDescription, Status, LinkSpeed, MacAddress |
        Format-Table -AutoSize |
        Out-String -Width 240
}

if ($LinuxIp) {
    Add-CommandOutput $lines "Test-NetConnection Linux port $Port" {
        Test-NetConnection -ComputerName $LinuxIp -Port $Port -InformationLevel Detailed
    }
    Add-CommandOutput $lines "Test-NetConnection Linux clipboard port $linuxClipboardPort" {
        Test-NetConnection -ComputerName $LinuxIp -Port $linuxClipboardPort -InformationLevel Detailed
    }
}

Add-Section $lines "PowerToys Processes"
Add-CommandOutput $lines "Running processes" {
    Get-Process |
        Where-Object {
            $_.ProcessName -match "PowerToys|Mouse|Borders"
        } |
        Select-Object ProcessName, Id, StartTime, CPU, Path |
        Format-Table -AutoSize |
        Out-String -Width 240
}

Add-CommandOutput $lines "Listening ports 15100-15101" {
    Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalPort -in @(15100, 15101) } |
        Select-Object LocalAddress, LocalPort, OwningProcess, CreationTime |
        Format-Table -AutoSize |
        Out-String -Width 240
}

Add-CommandOutput $lines "Owning process details for 15100-15101 listeners" {
    $listeners = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalPort -in @(15100, 15101) } |
        Select-Object -ExpandProperty OwningProcess -Unique

    if (-not $listeners) {
        "No listeners found on 15100 or 15101."
    } else {
        Get-Process -Id $listeners -ErrorAction SilentlyContinue |
            Select-Object ProcessName, Id, StartTime, Path |
            Format-Table -AutoSize |
            Out-String -Width 240
    }
}

Add-EventSummary -Lines $lines -StartTime $startTime
Add-PowerToysLogs -Lines $lines

try {
    $directory = Split-Path -Parent $OutputPath
    if ($directory -and -not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllLines($OutputPath, $lines)
    Write-Host ("Report written to " + $OutputPath)
} catch {
    Write-Host ("Failed to write report: " + $_.Exception.Message)
    exit 1
}
