# Read-only status check (no administrator needed):  powershell -ExecutionPolicy Bypass -File .\status-vdd.ps1
$hwid = 'Root\Parsec\VDA'
$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }
if ($dev) {
    $dev | Format-List FriendlyName, Status, Class, InstanceId, Problem
    foreach ($p in 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverProvider', 'DEVPKEY_Device_DriverDate') {
        $v = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $p -ErrorAction SilentlyContinue
        if ($v) { Write-Host ("{0,-32} {1}" -f $p, $v.Data) }
    }
} else {
    Write-Host 'Parsec Virtual Display Adapter: NOT installed'
}
Write-Host ''
$root = 'HKLM:\SOFTWARE\Parsec\vdd'
if (Test-Path $root) {
    Get-ChildItem $root | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath
        Write-Host ("custom resolution {0}: {1} x {2} @ {3}" -f $_.PSChildName, $p.width, $p.height, $p.hz)
    }
} else {
    Write-Host 'no custom resolutions configured'
}
Write-Host ''
Write-Host 'Monitors currently known to Windows:'
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue | ForEach-Object {
    $n = -join ($_.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
    Write-Host ("  {0}  {1}  active={2}" -f $_.InstanceName, $n, $_.Active)
}
