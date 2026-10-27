param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'

# Request a new native Windows managed-mode scan and wait for its completion.
# Reference: Microsoft WLAN API WlanScan / WlanRegisterNotification.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class BirriaWindowsScan {
    [StructLayout(LayoutKind.Sequential)]
    public struct Notification {
        public uint Source, Code;
        public Guid Interface;
        public uint Size;
        public IntPtr Data;
    }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate void Callback(ref Notification notification, IntPtr context);
    [DllImport("wlanapi.dll")]
    static extern uint WlanOpenHandle(uint version, IntPtr reserved, out uint negotiated, out IntPtr handle);
    [DllImport("wlanapi.dll")]
    static extern uint WlanScan(IntPtr handle, ref Guid id, IntPtr ssid, IntPtr ie, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    static extern uint WlanRegisterNotification(IntPtr handle, uint source, bool ignore,
        Callback callback, IntPtr context, IntPtr reserved, out uint previous);
    [DllImport("wlanapi.dll")]
    static extern uint WlanCloseHandle(IntPtr handle, IntPtr reserved);
    public static void Scan(Guid id) {
        uint version, previous;
        IntPtr handle;
        uint error = WlanOpenHandle(2, IntPtr.Zero, out version, out handle);
        if (error != 0) throw new Exception("WlanOpenHandle: " + error);
        using (var done = new ManualResetEvent(false)) {
            uint result = 0;
            Callback callback = (ref Notification n, IntPtr context) => {
                if (n.Interface == id && (n.Source & 8) != 0 && (n.Code == 7 || n.Code == 8)) {
                    result = n.Code;
                    done.Set();
                }
            };
            try {
                error = WlanRegisterNotification(handle, 8, false, callback,
                    IntPtr.Zero, IntPtr.Zero, out previous);
                if (error != 0) throw new Exception("WlanRegisterNotification: " + error);
                error = WlanScan(handle, ref id, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
                if (error != 0) throw new Exception("WlanScan: " + error);
                if (!done.WaitOne(10000)) throw new Exception("Timed out waiting for scan completion");
                if (result != 7) throw new Exception("Windows reported scan failure");
            } finally {
                WlanRegisterNotification(handle, 0, false, null, IntPtr.Zero, IntPtr.Zero, out previous);
                WlanCloseHandle(handle, IntPtr.Zero);
                GC.KeepAlive(callback);
            }
        }
    }
}
'@

$interfaceText = (& netsh.exe wlan show interfaces) -join "`n"
if ($LASTEXITCODE -ne 0) { throw $interfaceText }
$idMatch = [regex]::Match($interfaceText, '(?m)^\s*GUID\s*:\s*([0-9a-fA-F-]{36})')
if (-not $idMatch.Success) { throw 'No Windows WLAN interface found' }
$id = [guid]$idMatch.Groups[1].Value
$started = Get-Date
[BirriaWindowsScan]::Scan($id)
$raw = (& netsh.exe wlan show networks mode=bssid) -join "`n"
if ($LASTEXITCODE -ne 0) { throw $raw }

$records = [System.Collections.Generic.List[object]]::new()
$current = $null
$ssid = ''
foreach ($line in ($raw -split "`n")) {
    if ($line -match '^SSID\s+\d+\s*:\s*(.*)$') {
        $ssid = $Matches[1].Trim()
    } elseif ($line -match '^\s+BSSID\s+\d+\s*:\s*([0-9a-fA-F:]{17})\s*$') {
        if ($null -ne $current) { $records.Add([pscustomobject]$current) }
        $current = [ordered]@{ SSID = $ssid; BSSID = $Matches[1].ToLower(); Band = ''; Channel = 0; SignalPercent = 0; RadioType = ''; BasicRatesMbps = '' }
    } elseif ($null -ne $current) {
        if ($line -match '^\s+Band\s*:\s*(.+)$') { $current.Band = $Matches[1].Trim() }
        elseif ($line -match '^\s+Channel\s*:\s*(\d+)') { $current.Channel = [int]$Matches[1] }
        elseif ($line -match '^\s+Signal\s*:\s*(\d+)%') { $current.SignalPercent = [int]$Matches[1] }
        elseif ($line -match '^\s+Radio type\s*:\s*(.+)$') { $current.RadioType = $Matches[1].Trim() }
        elseif ($line -match '^\s+Basic rates \(Mbps\)\s*:\s*(.+)$') { $current.BasicRatesMbps = $Matches[1].Trim() }
    }
}
if ($null -ne $current) { $records.Add([pscustomobject]$current) }
$twoG = @($records | Where-Object { $_.Band -eq '2.4 GHz' } | Sort-Object Channel,SSID,BSSID)
if (-not $OutputDirectory) {
    $workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $OutputDirectory = Join-Path $workspace ('captures/windows_wifi_' + $started.ToString('yyyyMMdd_HHmmss'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists; refusing to overwrite' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$raw | Set-Content -LiteralPath (Join-Path $OutputDirectory 'raw-netsh.txt') -Encoding utf8
$interfaceText | Set-Content -LiteralPath (Join-Path $OutputDirectory 'interface.txt') -Encoding utf8
$report = [ordered]@{
    StartedAt = $started.ToString('o'); CompletedAt = (Get-Date).ToString('o')
    InterfaceGuid = $id.ToString(); NativeScanCompleted = $true
    Bssids24GHz = $twoG.Count; AccessPoints = $twoG
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'scan.json') -Encoding utf8
$twoG | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'ap24.csv') -NoTypeInformation -Encoding utf8
$twoG | Format-Table SSID,BSSID,Channel,SignalPercent,RadioType -AutoSize
Write-Output "Fresh Windows scan complete: $($twoG.Count) BSSIDs on 2.4 GHz"
Write-Output "Saved to: $OutputDirectory"
