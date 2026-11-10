# Camera experiment: does the e-ink waveform mode change the end-to-end latency?  (prototype tooling)
#   powershell -ExecutionPolicy Bypass -File tools\latency-modes.ps1 [-SecondsPerMode 25]
# It quits Padreen (if running), mirrors the virtual monitor with the Python sender (hidden window), flashes a big black/white
# square with slightly irregular timing on BOTH the PC reference window and the tablet, and cycles the tablet's waveform every
# -SecondsPerMode seconds:  DU(1)  A2-6frames(8)  A2-7(9)  A2-8(10)  A2-9(11)  A2/MIX(6)  DU4(5).
# The reference window shows the current phase and a countdown.  Film the reference window and the tablet together (phone propped
# up, no hand-holding) until the window disappears, then copy the video to docs\video\.
param([int]$SecondsPerMode = 25)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
$py = 'D:\Python3.8\python.exe'
$adb = 'D:\Android\android-sdk\platform-tools\adb.exe'
$waves = @(1, 8, 9, 10, 11, 6, 5)
$names = @('DU (1)', 'A2 6帧 (8)', 'A2 7帧 (9)', 'A2 8帧 (10)', 'A2 9帧 (11)', 'A2/MIX (6)', 'DU4 (5)')
$sched = ($waves | ForEach-Object { "$($_):$SecondsPerMode" }) -join ','
$start = 75                      # seconds after the mirror starts until the first mode (time to set up the phone)
$total = $start + $SecondsPerMode * $waves.Count + 12

# 1) Padreen must not hold the tablet connection
if (Get-Process padreen -ErrorAction SilentlyContinue) {
    Write-Host 'Quitting Padreen...'
    & $py "$root\host\poc\phctl.py" quit
    for ($i = 0; $i -lt 30 -and (Get-Process padreen -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 300 }
    if (Get-Process padreen -ErrorAction SilentlyContinue) { Get-Process padreen | Stop-Process -Force }
}
if (-not (Get-Process adb -ErrorAction SilentlyContinue)) {
    Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$adb`" -P 5037 start-server" } | Out-Null
    Start-Sleep -Seconds 3
}
New-Item -ItemType Directory -Force "$root\docs\video" | Out-Null
$flips = "$root\.tmp\modes-flips.csv"; $waveLog = "$root\.tmp\modes-waves.csv"
Remove-Item $flips, $waveLog -ErrorAction SilentlyContinue

Write-Host "Starting the mirror (waveform schedule $sched)..."
$mirrorArgs = @("$root\host\poc\mirror.py", '--vdd', '--fast', 'bw', '--settle-ms', '0', '--no-cursor', '--quiet', '--seconds', $total,
                '--wave-schedule', $sched, '--wave-start', $start, '--wave-log', $waveLog)
$t0 = Get-Date
$mirror = Start-Process -FilePath $py -ArgumentList $mirrorArgs -PassThru -WindowStyle Hidden
try {
    $ok = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        if (& $py "$root\host\poc\vdd.py" --status 2>&1 | Select-String -Pattern 'Parsec Virtual Display Adapter.*1872x1404') { $ok = $true; break }
    }
    if (-not $ok) { Write-Host 'The virtual monitor did not appear.' -ForegroundColor Red; exit 1 }
    Start-Sleep -Seconds 3
    $warm = [math]::Round($start - ((Get-Date) - $t0).TotalSeconds)
    $patSeconds = $warm + $SecondsPerMode * $waves.Count + 10
    Write-Host ''
    Write-Host "Pattern running for $patSeconds s: START FILMING NOW and keep both the PC window and the tablet in frame." -ForegroundColor Green
    & $py "$root\host\poc\latency_pattern.py" --mode blink --period 1.2 --jitter 0.8 --seconds $patSeconds --flip-log $flips `
        --warmup $warm --phase-seconds $SecondsPerMode --phase-names ($names -join ';')
} finally {
    if ($mirror -and -not $mirror.HasExited) { Stop-Process -Id $mirror.Id -Force }
    Write-Host 'Done. Copy the video to docs\video\ and tell me the file name.'
}
