param(
    [string]$PeerName = "fedora",
    [string]$PeerIp = "",
    [string]$SecurityKey = "",
    [ValidateSet("Auto", "TopLeft", "TopRight", "BottomLeft", "BottomRight")]
    [string]$PeerPosition = "Auto",
    [switch]$ClosePowerToys
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Text)
    Write-Host $Text
}

function Stop-PowerToysProcesses {
    $names = @(
        "PowerToys",
        "PowerToys.MouseWithoutBorders",
        "MouseWithoutBorders"
    )

    foreach ($name in $names) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

function Ensure-ArrayLength {
    param(
        [System.Collections.IList]$List,
        [int]$Length
    )

    while ($List.Count -lt $Length) {
        $List.Add("") | Out-Null
    }
}

function Parse-MachinePool {
    param([string]$Value)

    $entries = New-Object System.Collections.ArrayList
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ,$entries
    }

    foreach ($part in ($Value -split ",")) {
        $segments = $part -split ":", 2
        $name = if ($segments.Count -ge 1) { $segments[0] } else { "" }
        $id = if ($segments.Count -ge 2) { $segments[1] } else { "" }
        [void]$entries.Add([pscustomobject]@{
            Name = $name
            Id = $id
        })
    }

    return ,$entries
}

function Serialize-MachinePool {
    param([System.Collections.IList]$Entries)

    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($entry in $Entries) {
        $parts.Add(($entry.Name + ":" + $entry.Id)) | Out-Null
    }

    while ($parts.Count -lt 4) {
        $parts.Add(":") | Out-Null
    }

    while ($parts.Count -gt 4) {
        $parts.RemoveAt($parts.Count - 1)
    }

    return ($parts -join ",")
}

function Upsert-Name2IP {
    param(
        [string]$Value,
        [string]$Name,
        [string]$Ip
    )

    $parts = New-Object System.Collections.ArrayList
    $updated = $false
    foreach ($part in ($Value -split ",")) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            continue
        }

        $segments = $part -split ":", 2
        $entryName = if ($segments.Count -ge 1) { [string]$segments[0] } else { "" }
        if ($entryName.Equals($Name, [System.StringComparison]::OrdinalIgnoreCase)) {
            if (-not $updated) {
                [void]$parts.Add($Name + ":" + $Ip)
                $updated = $true
            }
            continue
        }

        [void]$parts.Add($part)
    }

    if (-not $updated) {
        [void]$parts.Add($Name + ":" + $Ip)
    }

    return ($parts -join ",")
}

function Get-PositionSlotIndex {
    param([string]$Position)

    switch ($Position) {
        "TopLeft" { return 0 }
        "TopRight" { return 1 }
        "BottomLeft" { return 2 }
        "BottomRight" { return 3 }
        default { return -1 }
    }
}

function Find-MachineSlot {
    param(
        [System.Collections.IList]$Matrix,
        [string]$Name
    )

    for ($i = 0; $i -lt $Matrix.Count; $i++) {
        if ([string]$Matrix[$i] -ceq $Name) {
            return $i
        }
    }

    return -1
}

function Resolve-PeerSlot {
    param(
        [System.Collections.IList]$Matrix,
        [int]$SelfSlot,
        [int]$ExistingPeerSlot,
        [string]$Position
    )

    if ($Position -ne "Auto") {
        return (Get-PositionSlotIndex -Position $Position)
    }

    if ($ExistingPeerSlot -ge 0 -and $ExistingPeerSlot -ne $SelfSlot) {
        return $ExistingPeerSlot
    }

    for ($i = 0; $i -lt $Matrix.Count; $i++) {
        if ($i -eq $SelfSlot) {
            continue
        }
        if ([string]::IsNullOrWhiteSpace([string]$Matrix[$i])) {
            return $i
        }
    }

    return $(if ($SelfSlot -ne 1) { 1 } else { 0 })
}

$settingsPath = Join-Path $env:LOCALAPPDATA "Microsoft\PowerToys\MouseWithoutBorders\settings.json"
if (-not (Test-Path $settingsPath)) {
    throw "Settings file not found: $settingsPath"
}

if ($ClosePowerToys) {
    Write-Info "Stopping PowerToys processes..."
    Stop-PowerToysProcesses
}

$jsonText = Get-Content -Path $settingsPath -Raw -Encoding UTF8
$settings = $jsonText | ConvertFrom-Json
$props = $settings.properties

if (-not [string]::IsNullOrWhiteSpace($SecurityKey)) {
    if ($null -eq $props.SecurityKey) {
        $props | Add-Member -NotePropertyName SecurityKey -NotePropertyValue ([pscustomobject]@{
            value = $SecurityKey
        })
    } else {
        $props.SecurityKey.value = $SecurityKey
    }
}

if ($null -eq $props.MachineMatrixString) {
    throw "MachineMatrixString missing from settings.json"
}

$matrix = New-Object System.Collections.ArrayList
foreach ($item in $props.MachineMatrixString) {
    [void]$matrix.Add([string]$item)
}
Ensure-ArrayLength -List $matrix -Length 4

$selfName = [string]$env:COMPUTERNAME
$selfSlot = Find-MachineSlot -Matrix $matrix -Name $selfName
$existingPeerSlot = Find-MachineSlot -Matrix $matrix -Name $PeerName

if ($selfSlot -lt 0) {
    $selfSlot = 0
}

for ($i = 0; $i -lt $matrix.Count; $i++) {
    $entry = [string]$matrix[$i]
    if ($entry.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase) -or
        $entry.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {
        $matrix[$i] = ""
    }
}

$matrix[$selfSlot] = $selfName

$slot = Resolve-PeerSlot -Matrix $matrix -SelfSlot $selfSlot -ExistingPeerSlot $existingPeerSlot -Position $PeerPosition
if ($slot -lt 0 -or $slot -ge $matrix.Count) {
    throw "Resolved peer slot is out of range."
}

if ($slot -eq $selfSlot) {
    throw "Peer position '$PeerPosition' collides with the local Windows machine slot."
}

$displaced = [string]$matrix[$slot]
$matrix[$slot] = $PeerName

if (-not [string]::IsNullOrWhiteSpace($displaced) -and
    -not $displaced.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase) -and
    -not $displaced.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {
    $fallbackSlot = -1
    if ($existingPeerSlot -ge 0 -and $existingPeerSlot -ne $slot -and $existingPeerSlot -ne $selfSlot -and
        [string]::IsNullOrWhiteSpace([string]$matrix[$existingPeerSlot])) {
        $fallbackSlot = $existingPeerSlot
    } else {
        for ($i = 0; $i -lt $matrix.Count; $i++) {
            if ($i -eq $selfSlot -or $i -eq $slot) {
                continue
            }
            if ([string]::IsNullOrWhiteSpace([string]$matrix[$i])) {
                $fallbackSlot = $i
                break
            }
        }
    }

    if ($fallbackSlot -lt 0) {
        throw "Cannot place '$PeerName' at '$PeerPosition' without overwriting existing machine '$displaced'."
    }

    $matrix[$fallbackSlot] = $displaced
}

$props.MachineMatrixString = @($matrix)

$existingPool = ""
if ($null -ne $props.MachinePool -and $null -ne $props.MachinePool.value) {
    $existingPool = [string]$props.MachinePool.value
} elseif ($null -eq $props.MachinePool) {
    $props | Add-Member -NotePropertyName MachinePool -NotePropertyValue ([pscustomobject]@{
        value = ""
    })
}

$selfId = "NONE"
$otherEntries = New-Object System.Collections.ArrayList
foreach ($entry in (Parse-MachinePool -Value $existingPool)) {
    $name = [string]$entry.Name
    $id = [string]$entry.Id
    if ([string]::IsNullOrWhiteSpace($name) -and [string]::IsNullOrWhiteSpace($id)) {
        continue
    }

    if ($name.Equals($selfName, [System.StringComparison]::OrdinalIgnoreCase)) {
        if (-not [string]::IsNullOrWhiteSpace($id)) {
            $selfId = $id
        }
        continue
    }

    if ($name.Equals($PeerName, [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }

    if ($otherEntries.Count -lt 2) {
        [void]$otherEntries.Add([pscustomobject]@{
            Name = $name
            Id = $id
        })
    }
}

$entries = New-Object System.Collections.ArrayList
[void]$entries.Add([pscustomobject]@{
    Name = $selfName
    Id = $selfId
})
[void]$entries.Add([pscustomobject]@{
    Name = $PeerName
    Id = "NONE"
})
foreach ($entry in $otherEntries) {
    if ($entries.Count -ge 4) {
        break
    }
    [void]$entries.Add($entry)
}

$props.MachinePool.value = Serialize-MachinePool -Entries $entries

if (-not [string]::IsNullOrWhiteSpace($PeerIp)) {
    if ($null -eq $props.Name2IP) {
        $props | Add-Member -NotePropertyName Name2IP -NotePropertyValue ([pscustomobject]@{
            value = ""
        })
    }
    $props.Name2IP.value = Upsert-Name2IP -Value ([string]$props.Name2IP.value) -Name $PeerName -Ip $PeerIp
}

$backupPath = $settingsPath + ".bak-" + (Get-Date -Format "yyyyMMdd-HHmmss")
Copy-Item -Path $settingsPath -Destination $backupPath -Force

$settings | ConvertTo-Json -Depth 8 | Set-Content -Path $settingsPath -Encoding UTF8

Write-Info ("Updated settings: " + $settingsPath)
Write-Info ("Backup written: " + $backupPath)
if (-not [string]::IsNullOrWhiteSpace($SecurityKey)) {
    Write-Info ("SecurityKey synchronized for " + $PeerName)
}
Write-Info ("PeerPosition: " + $PeerPosition)
Write-Info ("MachineMatrixString: " + (($props.MachineMatrixString | ForEach-Object { [string]$_ }) -join ","))
Write-Info ("MachinePool: " + [string]$props.MachinePool.value)
if (-not [string]::IsNullOrWhiteSpace($PeerIp)) {
    Write-Info ("Name2IP: " + [string]$props.Name2IP.value)
}
