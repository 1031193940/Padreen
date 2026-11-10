# Remove everything install-vdd.ps1 added.  MUST be run from an elevated (Administrator) PowerShell.
#   powershell -ExecutionPolicy Bypass -File .\uninstall-vdd.ps1
$ErrorActionPreference = 'Continue'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here 'uninstall-log.txt'
Start-Transcript -Path $log -Force | Out-Null

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'ERROR: please run this script from an elevated (Run as administrator) PowerShell.' -ForegroundColor Red
    Stop-Transcript | Out-Null
    exit 1
}

$nef = Join-Path $here 'nefconw.exe'
$inf = Join-Path $here 'driver\mm.inf'
$hwid = 'Root\Parsec\VDA'
$cls = '4D36E968-E325-11CE-BFC1-08002BE10318'

function Run($exe, $argList) {
    Write-Host ("> {0} {1}" -f (Split-Path $exe -Leaf), ($argList -join ' '))
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
    Write-Host ("  exit code {0}" -f $p.ExitCode)
}

Run $nef @('--remove-device-node', '--hardware-id', $hwid, '--class-guid', "`"$cls`"")
Run $nef @('--uninstall-driver', '--inf-path', "`"$inf`"")

# registry: only the entries we created
$root = 'HKLM:\SOFTWARE\Parsec\vdd'
foreach ($i in '0', '1') {
    $k = Join-Path $root $i
    if (Test-Path $k) { Remove-Item $k -Recurse -Force; Write-Host "removed registry key $k" }
}
if ((Test-Path $root) -and -not (Get-ChildItem $root -ErrorAction SilentlyContinue) -and -not (Get-ItemProperty $root -ErrorAction SilentlyContinue | Get-Member -MemberType NoteProperty | Where-Object { $_.Name -notmatch '^PS' })) {
    Remove-Item $root -Force
    Write-Host "removed empty registry key $root"
    $parent = 'HKLM:\SOFTWARE\Parsec'
    if ((Test-Path $parent) -and -not (Get-ChildItem $parent -ErrorAction SilentlyContinue)) { Remove-Item $parent -Force; Write-Host "removed empty registry key $parent" }
}

$left = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }
if ($left) { Write-Host 'WARNING: the adapter is still listed; reboot Windows once and check Device Manager.' -ForegroundColor Yellow }
else { Write-Host 'Uninstalled.' -ForegroundColor Green }
Stop-Transcript | Out-Null
