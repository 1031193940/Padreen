# Assemble the ready-to-use Padreen folder:  dist\Padreen\
#   powershell -ExecutionPolicy Bypass -File tools\make-dist.ps1 [-Build]
# -Build also rebuilds padreen.exe and the tablet APK first.  Everything is copied (nothing is installed or changed).
param([switch]$Build)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ps = "$PSHOME\powershell.exe"
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
if ($Build) {
    & $ps -NoProfile -ExecutionPolicy Bypass -File "$root\android\app\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'APK build failed' }
    Copy-Item "$root\android\app\build\padreen.apk" "$root\host\padreen\build\padreen.apk" -Force
    & $ps -NoProfile -ExecutionPolicy Bypass -File "$root\host\padreen\build.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'Padreen build failed' }
}
$out = "$root\dist\Padreen"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out, "$out\adb", "$out\driver" | Out-Null
$b = "$root\host\padreen\build"
Copy-Item "$b\padreen.exe", "$b\padreen.apk", "$b\padreen-white.png" $out
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') { Copy-Item "D:\Android\android-sdk\platform-tools\$f" "$out\adb" }
Copy-Item "$root\tools\vdd\pkg\*" "$out\driver" -Recurse -Exclude 'install-log.txt'
Copy-Item "$root\host\padreen\padreen.ini.sample" "$out\padreen.ini.sample"
Copy-Item "$root\host\padreen\README-dist.txt" "$out\使用说明.txt"
Get-ChildItem $out -Recurse -File | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($out.Length + 1) }
Write-Host "dist ready: $out" -ForegroundColor Green
