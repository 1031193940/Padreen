# Assemble the ready-to-use Padreen folder:  dist\Padreen\
#   powershell -ExecutionPolicy Bypass -File tools\make-dist.ps1 [-Build]
# -Build also rebuilds padreen.exe and the tablet APK first.  Everything is copied (nothing is installed or changed).
# -SkipDriverCheck   do not test the driver download against Parsec's server (builds without a network connection; never for a release)
param([switch]$Build, [switch]$SkipDriverCheck)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'      # not $PSHOME: that is pwsh's folder when this runs in PowerShell 7
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
if ($Build) {
    & $ps -NoProfile -ExecutionPolicy Bypass -File "$root\android\app\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'APK build failed' }
    Copy-Item "$root\android\app\build\padreen.apk" "$root\host\padreen\build\padreen.apk" -Force
    & $ps -NoProfile -ExecutionPolicy Bypass -File "$root\host\padreen\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Padreen build failed' }
}
$out = "$root\dist\Padreen"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out, "$out\adb" | Out-Null
$b = "$root\host\padreen\build"
Copy-Item "$b\padreen.exe", "$b\padreen.apk", "$b\padreen-white.png" $out
$adbDir = Find-AdbDir
if (-not $adbDir) { throw 'adb.exe (with AdbWinApi.dll and AdbWinUsbApi.dll) not found in the Android SDK platform-tools: set ANDROID_HOME (see tools\setup-build-env.ps1)' }
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') { Copy-Item "$adbDir\$f" "$out\adb" }
Copy-Item "$root\host\padreen\padreen.ini.sample" "$out\padreen.ini.sample"
Copy-Item "$root\host\padreen\README-dist.txt" "$out\使用说明.txt"
# The virtual display driver is Parsec's software and is NOT part of any package: no permission to redistribute it has been found.  padreen.exe fetches it
# from Parsec's own download address when the user agrees (first start / tray menu) and checks every file against SHA-256 values built into the exe.
# A release must be able to do that, so prove it now with the finished exe against Parsec's live downloads (it writes below .tmp only):
if (-not $SkipDriverCheck) {
    $cache = Join-Path $env:TEMP 'vdd-check'
    if (Test-Path -LiteralPath $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }
    $log = "$cache.log"
    $check = Start-Process -FilePath "$out\padreen.exe" -ArgumentList '--fetch-driver', '--cache', "`"$cache`"" -RedirectStandardError $log -WindowStyle Hidden -Wait -PassThru
    $why = if (Test-Path -LiteralPath $log) { (Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Last 3) -join "`n" } else { '' }
    Remove-Item -LiteralPath $cache, $log -Recurse -Force -ErrorAction SilentlyContinue
    if ($check.ExitCode -ne 0) { throw "padreen.exe could not fetch the driver from Parsec (--fetch-driver exit code $($check.ExitCode)): Parsec changed its files, or there is no network.  Last log lines:`n$why`nThe pinned values are in host\padreen\src\driver.cpp (and tools\fetch-vdd.ps1)." }
    Write-Host 'driver download check: padreen.exe fetched and verified the driver from Parsec' -ForegroundColor Green
}
# ... and nothing of Parsec's may be inside the package
$parsecFiles = 'nefconw.exe', 'mm.dll', 'mm.inf', 'mm.cat', 'parsec-vdd-*.exe', 'devcon.exe'
$found = @(Get-ChildItem $out -Recurse -File | Where-Object { $n = $_.Name; @($parsecFiles | Where-Object { $n -like $_ }).Count -gt 0 })
if ($found.Count) { throw "Parsec's driver files must not be in the package (no permission to redistribute them has been found): $($found.FullName -join ', ')" }
Get-ChildItem $out -Recurse -File | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($out.Length + 1) }
Write-Host "dist ready: $out" -ForegroundColor Green
