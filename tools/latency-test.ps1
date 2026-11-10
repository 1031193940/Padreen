# Latency test session (prototype): starts the mirror to the virtual monitor, then the flipping-square pattern.
# Film the reference window (on your PC monitor) and the tablet TOGETHER with a phone, then Esc to stop.
#   powershell -ExecutionPolicy Bypass -File tools\latency-test.ps1 [-Seconds 45] [-Period 1.2] [-Mode blink|counter]
param(
    [int]$Seconds = 45,
    [double]$Period = 1.2,
    [ValidateSet('blink', 'counter')][string]$Mode = 'blink',
    [ValidateSet('bw', 'gray')][string]$Fast = 'bw'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
$py = 'D:\Python3.8\python.exe'
$adb = 'D:\Android\android-sdk\platform-tools\adb.exe'
if (-not (Get-Process adb -ErrorAction SilentlyContinue)) {
    Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$adb`" -P 5037 start-server" } | Out-Null
    Start-Sleep -Seconds 3
}
if (-not (& $adb devices 2>&1 | Select-String -Pattern "`tdevice$")) { Write-Host 'No tablet found over USB.' -ForegroundColor Yellow; exit 1 }

Write-Host 'Starting the mirror (virtual monitor)...'
$mirrorArgs = @("$root\host\poc\mirror.py", '--vdd', '--fast', $Fast, '--settle-ms', '1200', '--quiet', '--seconds', ($Seconds + 30), '--no-cursor')
if ($Fast -eq 'gray') { $mirrorArgs += @('--mode', 'du4') }
$mirror = Start-Process -FilePath $py -ArgumentList $mirrorArgs -PassThru -WindowStyle Minimized
try {
    # wait for the virtual monitor to show up
    $ok = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        if (& $py "$root\host\poc\vdd.py" --status 2>&1 | Select-String -Pattern 'Parsec Virtual Display Adapter.*1872x1404') { $ok = $true; break }
    }
    if (-not $ok) { Write-Host 'The virtual monitor did not appear.' -ForegroundColor Red; exit 1 }
    Start-Sleep -Seconds 3
    Write-Host ''
    Write-Host "Pattern running for $Seconds s ($Mode). Film the reference window (on your PC monitor) together with the tablet." -ForegroundColor Green
    Write-Host 'Press Esc (with the pattern window focused) to stop early.'
    & $py "$root\host\poc\latency_pattern.py" --mode $Mode --period $Period --seconds $Seconds
} finally {
    if ($mirror -and -not $mirror.HasExited) { Stop-Process -Id $mirror.Id -Force }
    Write-Host 'Done. The virtual monitor disappears within a second.'
}
