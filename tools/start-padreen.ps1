# Plug-and-use launcher for the Python prototype (no administrator needed):
#   tablet over USB  ->  virtual monitor 1872x1404 (Parsec VDD)  ->  e-ink display
# Stop with Ctrl-C or by closing the window: the virtual monitor disappears by itself within ~1 s.
#   -Fast bw    1-bit DU fast mode + gray settle (default, fastest)
#   -Fast gray  4-gray DU4 mode (slower, ~0.6 s per refresh, nicer text)
#   -Minutes N  stop after N minutes (default 0 = run until stopped)
param(
    [ValidateSet('bw', 'gray')][string]$Fast = 'bw',
    [int]$Minutes = 0
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$env:TEMP = Join-Path $root '.tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$adb = 'D:\Android\android-sdk\platform-tools\adb.exe'
# adb's background server must be started through WMI: started from a pipe-captured shell it keeps the pipes open
if (-not (Get-Process adb -ErrorAction SilentlyContinue)) {
    Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$adb`" -P 5037 start-server" } | Out-Null
    Start-Sleep -Seconds 3
}
$dev = & $adb devices 2>&1 | Select-String -Pattern "`tdevice$"
if (-not $dev) {
    Write-Host 'No tablet found. Connect the MatePad Paper with the USB cable and allow USB debugging, then run this again.' -ForegroundColor Yellow
    exit 1
}
if (-not (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\Parsec\VDA' })) {
    Write-Host 'The virtual display driver is not installed: run tools\vdd\pkg\install-vdd.ps1 once from an administrator PowerShell.' -ForegroundColor Yellow
    exit 1
}
Write-Host "Tablet found. Starting ($Fast mode). A third monitor (1872x1404) will appear to the right of your other screens."
$py = 'D:\Python3.8\python.exe'
$pyArgs = @("$root\host\poc\mirror.py", '--vdd', '--fast', $Fast, '--settle-ms', '1200', '--quiet')
if ($Minutes -gt 0) { $pyArgs += @('--seconds', ($Minutes * 60)) } else { $pyArgs += @('--seconds', '0') }
if ($Fast -eq 'gray') { $pyArgs += @('--mode', 'du4') }
& $py @pyArgs
