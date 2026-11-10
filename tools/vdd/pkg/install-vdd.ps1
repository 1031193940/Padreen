# Install the Parsec Virtual Display Driver (0.41, Microsoft-signed IddCx user-mode driver) for Padreen.
# MUST be run from an elevated (Administrator) PowerShell:   powershell -ExecutionPolicy Bypass -File .\install-vdd.ps1
#
# What this script changes on the PC (nothing else is touched):
#   1. Creates ONE root-enumerated device "Parsec Virtual Display Adapter" (hardware id Root\Parsec\VDA, class Display).
#      It has no physical hardware and creates NO monitor by itself.
#   2. Stages the driver package driver\mm.inf into the Windows driver store (C:\Windows\System32\DriverStore\...)
#      and Windows copies mm.dll to C:\Windows\System32\drivers\UMDF\.   (Windows driver mechanism, cannot live on D:)
#   3. Writes the custom resolution(s) the driver reads at start-up:
#        HKLM\SOFTWARE\Parsec\vdd\0 = 1872 x 1404 @ 60   (the e-ink panel, landscape)
#        HKLM\SOFTWARE\Parsec\vdd\1 = 1404 x 1872 @ 60   (portrait, for later)
# Undo everything with uninstall-vdd.ps1 (also needs Administrator).
param(
    [int]$Width = 1872,
    [int]$Height = 1404,
    [int]$Hz = 60
)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here 'install-log.txt'
Start-Transcript -Path $log -Force | Out-Null

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; Stop-Transcript | Out-Null; exit 1 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'Please run this script from an elevated (Run as administrator) PowerShell.'
}

$nef = Join-Path $here 'nefconw.exe'
$inf = Join-Path $here 'driver\mm.inf'
$hwid = 'Root\Parsec\VDA'
$cls = '4D36E968-E325-11CE-BFC1-08002BE10318'

# 0) refuse to continue unless every binary carries a valid signature
foreach ($f in @($nef, (Join-Path $here 'driver\mm.dll'), (Join-Path $here 'driver\mm.cat'))) {
    if (-not (Test-Path $f)) { Fail "missing file: $f" }
    $sig = Get-AuthenticodeSignature $f
    if ($sig.Status -ne 'Valid') { Fail "signature of $f is not valid ($($sig.Status))" }
    Write-Host ("signature OK : {0}  ({1})" -f (Split-Path $f -Leaf), (($sig.SignerCertificate.Subject -split ',')[0]))
}

function Run($exe, $argList) {
    Write-Host ("> {0} {1}" -f (Split-Path $exe -Leaf), ($argList -join ' '))
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
    Write-Host ("  exit code {0}" -f $p.ExitCode)
    return $p.ExitCode
}

# 1) device node (remove a stale one first, like the vendor's own install script)
$existing = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }
if ($existing) {
    Write-Host 'An adapter with this hardware id already exists - removing it first (clean re-install).'
    Run $nef @('--remove-device-node', '--hardware-id', $hwid, '--class-guid', "`"$cls`"") | Out-Null
}
$rc = Run $nef @('--create-device-node', '--class-name', 'Display', '--class-guid', "`"$cls`"", '--hardware-id', $hwid)
if ($rc -ne 0) { Fail "creating the device node failed (exit code $rc)" }

# 2) driver package
$rc = Run $nef @('--install-driver', '--inf-path', "`"$inf`"")
if ($rc -ne 0) { Fail "installing the driver failed (exit code $rc)" }

# 3) custom resolutions read by the driver at start-up (DWORD values, decimal)
$root = 'HKLM:\SOFTWARE\Parsec\vdd'
function SetRes($index, $w, $h, $hz) {
    $k = Join-Path $root $index
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -Path $k -Name 'width'  -Value $w  -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name 'height' -Value $h  -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name 'hz'     -Value $hz -PropertyType DWord -Force | Out-Null
    Write-Host ("registry     : {0} = {1} x {2} @ {3}" -f $k, $w, $h, $hz)
}
SetRes '0' $Width $Height $Hz
SetRes '1' $Height $Width $Hz

# 4) report
Start-Sleep -Seconds 2
$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }
if ($dev) {
    Write-Host ''
    Write-Host ("adapter      : {0}   status = {1}" -f $dev.FriendlyName, $dev.Status) -ForegroundColor Green
} else {
    Write-Host 'WARNING: the adapter is not visible yet - open Device Manager > Display adapters and check it.' -ForegroundColor Yellow
}
Write-Host ''
Write-Host 'Done. No virtual monitor exists yet: Padreen plugs one in only while it is running and the tablet is connected.'
Write-Host "Log: $log"
Stop-Transcript | Out-Null
