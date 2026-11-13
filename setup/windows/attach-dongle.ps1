<#
.SYNOPSIS
  Share a BirriaSDR dongle with WSL2 through usbipd-win and attach it.

.DESCRIPTION
  Finds the MT7961 dongle (0e8d:7961 or 3574:6211), binds it with --force the
  first time (one UAC prompt), asks for a replug after that first bind so the
  chip starts without the firmware Windows' Wi-Fi driver loaded, then attaches
  it to WSL. Run it from an ordinary PowerShell window with a WSL terminal
  open. Bus IDs change with what is plugged in, so the script always looks
  the dongle up again instead of remembering one.

.PARAMETER BusId
  Which dongle to use when more than one is connected.

.PARAMETER AutoAttach
  Keep running and re-attach the dongle after every replug (usbipd's
  --auto-attach). Leave the window open; Ctrl+C stops it.

.PARAMETER CheckOnly
  Only show what was found; change nothing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\attach-dongle.ps1
#>
[CmdletBinding()]
param(
    [string]$BusId,
    [switch]$AutoAttach,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$Ids = @('VID_0E8D&PID_7961', 'VID_3574&PID_6211')

$usbipd = Get-Command usbipd.exe -ErrorAction SilentlyContinue
if ($usbipd) {
    $usbipd = $usbipd.Source
} elseif (Test-Path 'C:\Program Files\usbipd-win\usbipd.exe') {
    $usbipd = 'C:\Program Files\usbipd-win\usbipd.exe'
} else {
    Write-Host 'usbipd-win is not installed. Install it, open a new PowerShell, and rerun:'
    Write-Host '  winget install --exact dorssel.usbipd-win'
    exit 1
}

function Get-Dongles {
    $json = & $usbipd state
    if ($LASTEXITCODE -ne 0) { throw "usbipd state failed ($LASTEXITCODE)" }
    $all = ($json | Out-String | ConvertFrom-Json).Devices
    $all | Where-Object {
        $d = $_
        $d.BusId -and ($Ids | Where-Object { $d.InstanceId.ToUpper().Contains($_) })
    }
}

function Select-Dongle {
    $found = @(Get-Dongles)
    if ($found.Count -eq 0) {
        Write-Host 'No BirriaSDR dongle (0e8d:7961 or 3574:6211) is connected.'
        Write-Host 'Plug it into a USB 3 port (blue or SS-marked) and rerun.'
        exit 1
    }
    if ($BusId) {
        $pick = @($found | Where-Object { $_.BusId -eq $BusId })
        if ($pick.Count -ne 1) {
            Write-Host "No dongle on bus ID $BusId. Connected:"
            $found | ForEach-Object { Write-Host "  $($_.BusId)  $($_.Description)" }
            exit 1
        }
        return $pick[0]
    }
    if ($found.Count -gt 1) {
        Write-Host 'More than one dongle is connected. Rerun with -BusId <id>:'
        $found | ForEach-Object { Write-Host "  $($_.BusId)  $($_.Description)" }
        exit 1
    }
    return $found[0]
}

function Show-Dongle($d) {
    $shared = if ($d.PersistedGuid) { if ($d.IsForced) { 'shared (forced)' } else { 'shared (not forced)' } } else { 'not shared' }
    $attached = if ($d.ClientIPAddress) { "attached to $($d.ClientIPAddress)" } else { 'not attached' }
    Write-Host "Dongle on bus ID $($d.BusId): $($d.Description)"
    Write-Host "  $shared, $attached"
}

$dongle = Select-Dongle
Show-Dongle $dongle
if ($CheckOnly) { exit 0 }

if ($dongle.ClientIPAddress -and -not $AutoAttach) {
    Write-Host 'Already attached. In WSL, run setup/check-dongle.sh.'
    exit 0
}

if (-not ($dongle.PersistedGuid -and $dongle.IsForced)) {
    # --force puts usbipd's stub driver on the dongle, so Windows' Wi-Fi driver
    # never loads firmware onto it again, including after replugs.
    Write-Host "Binding bus ID $($dongle.BusId) with --force (approve the UAC prompt)..."
    $p = Start-Process -FilePath $usbipd -ArgumentList @('bind', '--force', '--busid', $dongle.BusId) -Verb RunAs -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Host "usbipd bind failed (exit $($p.ExitCode)). From an admin PowerShell run:"
        Write-Host "  usbipd bind --force --busid $($dongle.BusId)"
        exit 1
    }
    Write-Host ''
    Write-Host 'Bound. Windows may already have loaded its Wi-Fi firmware onto the chip,'
    Write-Host 'which BirriaSDR cannot clear. Unplug the dongle, wait 5 seconds, plug it'
    Write-Host 'back into the same USB 3 port, and wait until Windows stops chiming.'
    Read-Host 'Press Enter when it is plugged back in'
    Start-Sleep -Seconds 2
    $BusId = $null
    $dongle = Select-Dongle
    Show-Dongle $dongle
    if (-not ($dongle.PersistedGuid -and $dongle.IsForced)) {
        Write-Host 'The replugged dongle does not show as shared (forced). It may be on a'
        Write-Host 'different port now; rerun this script.'
        exit 1
    }
}

$attachArgs = @('attach', '--wsl', '--busid', $dongle.BusId)
if ($AutoAttach) {
    $attachArgs += '--auto-attach'
    Write-Host 'Attaching and re-attaching after replugs. Keep this window open; Ctrl+C stops it.'
} else {
    Write-Host "Attaching bus ID $($dongle.BusId) to WSL..."
}
& $usbipd @attachArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host 'Attach failed. Check that a WSL 2 terminal is open (WSL must be running),'
    Write-Host 'then rerun. If usbipd says "Device in error state", unplug and replug the'
    Write-Host 'dongle and rerun.'
    exit 1
}
Write-Host 'Attached. In WSL, run setup/check-dongle.sh (add --capture for a test recording).'
