<#
.SYNOPSIS
  Share a BirriaSDR dongle with WSL2 through usbipd-win and attach it.

.DESCRIPTION
  Finds the MT7961 dongle (0e8d:7961 or 3574:6211), binds it with --force the
  first time (one UAC prompt), waits for a replug after that first bind so the
  chip starts without any firmware Windows' Wi-Fi driver loaded, then attaches
  it to WSL. Run it from an ordinary PowerShell window, or from WSL with
  powershell.exe, with a WSL terminal open. Bus IDs change with what is
  plugged in, so the script always looks the dongle up again instead of
  remembering one.

.PARAMETER BusId
  Which dongle to use when more than one is connected.

.PARAMETER AutoAttach
  Keep running and re-attach the dongle after every replug (usbipd's
  --auto-attach). Leave the window open; Ctrl+C stops it.

.PARAMETER CheckOnly
  Only show what was found; change nothing.

.PARAMETER ReplugTimeout
  Seconds to wait for the unplug and for the replug after the first bind
  (default 300 each).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\attach-dongle.ps1
#>
[CmdletBinding()]
param(
    [string]$BusId,
    [switch]$AutoAttach,
    [switch]$CheckOnly,
    [int]$ReplugTimeout = 300
)

$ErrorActionPreference = 'Stop'
$Ids = @('VID_0E8D&PID_7961', 'VID_3574&PID_6211')
$PollMilliseconds = 500
$SettleSeconds = 3

function Find-Usbipd {
    $cmd = Get-Command usbipd.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # A PowerShell started from WSL keeps the PATH WSL started with, so a
    # usbipd-win installed since then is only found at its install path.
    $default = 'C:\Program Files\usbipd-win\usbipd.exe'
    if (Test-Path $default) { return $default }
    return $null
}

function Get-Dongles {
    $json = & $script:usbipd state
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

# Wait for the dongle on $OldBusId to disappear, then for a dongle to appear
# on a bus that had none while it was unplugged. Polling instead of asking for
# Enter means it can't go ahead before the cable has actually been pulled,
# and it also works when no one can type into the window.
function Wait-Replug([string]$OldBusId, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    Write-Host "Waiting up to $TimeoutSeconds s for the dongle to be unplugged..."
    while (@(Get-Dongles | Where-Object { $_.BusId -eq $OldBusId }).Count -gt 0) {
        if ((Get-Date) -gt $deadline) { return $null }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
    $others = @(Get-Dongles | ForEach-Object { $_.BusId })
    Write-Host 'Unplugged. Wait 5 seconds, then plug it back into the same USB 3 port.'
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($true) {
        $back = @(Get-Dongles | Where-Object { $others -notcontains $_.BusId })
        if ($back.Count -gt 0) { break }
        if ((Get-Date) -gt $deadline) { return $null }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
    # Give Windows a moment to finish installing the stub driver.
    Start-Sleep -Seconds $SettleSeconds
    $back = @(Get-Dongles | Where-Object { $others -notcontains $_.BusId })
    if ($back.Count -eq 0) { return $null }
    return $back[0]
}

function Invoke-ForcedBind($d) {
    # --force puts usbipd's stub driver on the dongle, so Windows' Wi-Fi driver
    # never loads firmware onto it again, including after replugs.
    Write-Host ''
    Write-Host "Binding bus ID $($d.BusId) needs administrator rights. A Windows UAC prompt"
    Write-Host 'for "usbipd-win" opens in a few seconds: approve it. If it does not come'
    Write-Host 'to the front, look for a flashing shield icon on the taskbar.'
    foreach ($n in 3, 2, 1) { Write-Host "  $n..."; Start-Sleep -Seconds 1 }
    try {
        $p = Start-Process -FilePath $script:usbipd -ArgumentList @('bind', '--force', '--busid', $d.BusId) -Verb RunAs -Wait -PassThru
    } catch {
        Write-Host 'The UAC prompt was declined or closed, so nothing changed. Rerun this'
        Write-Host 'script and approve the prompt.'
        exit 1
    }
    if ($p.ExitCode -ne 0) {
        Write-Host "usbipd bind failed (exit $($p.ExitCode)). From an admin PowerShell run:"
        Write-Host "  & '$script:usbipd' bind --force --busid $($d.BusId)"
        exit 1
    }
}

function Invoke-Main {
    $script:usbipd = Find-Usbipd
    if (-not $script:usbipd) {
        Write-Host 'usbipd-win is not installed. Install it, open a new PowerShell, and rerun:'
        Write-Host '  winget install --exact dorssel.usbipd-win'
        exit 1
    }

    $dongle = Select-Dongle
    Show-Dongle $dongle
    if ($CheckOnly) { exit 0 }

    if ($dongle.ClientIPAddress -and -not $AutoAttach) {
        Write-Host 'Already attached. In WSL, run setup/check-dongle.sh.'
        exit 0
    }

    if (-not ($dongle.PersistedGuid -and $dongle.IsForced)) {
        Invoke-ForcedBind $dongle
        Write-Host ''
        Write-Host 'Bound. Windows may already have loaded its Wi-Fi firmware onto the chip,'
        Write-Host 'which BirriaSDR cannot clear, so the dongle needs one replug now.'
        Write-Host 'Unplug it.'
        $dongle = Wait-Replug $dongle.BusId $ReplugTimeout
        if (-not $dongle) {
            Write-Host 'No unplug and replug seen in time. The bind is kept: replug the'
            Write-Host 'dongle, then rerun this script, which goes straight to attaching.'
            exit 1
        }
        Show-Dongle $dongle
        if (-not ($dongle.PersistedGuid -and $dongle.IsForced)) {
            Write-Host 'The replugged dongle does not show as shared (forced), probably because'
            Write-Host 'it went into a different port. Rerun this script to bind it there.'
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
    & $script:usbipd @attachArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host 'Attach failed. Check that a WSL 2 terminal is open (WSL must be running),'
        Write-Host 'then rerun. If usbipd says "Device in error state", unplug and replug the'
        Write-Host 'dongle and rerun.'
        exit 1
    }
    Write-Host 'Attached. In WSL, run setup/check-dongle.sh (add --capture for a test recording).'
}

# Dot-sourcing (. .\attach-dongle.ps1) loads the functions without running.
if ($MyInvocation.InvocationName -ne '.') { Invoke-Main }
