# Build the Windows installer:  dist\Padreen-Setup-<version>.exe  (Inno Setup 7: tools\setup-build-env.ps1 installs it to D:\Tools\InnoSetup)
#   powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-Build] [-SkipDriverCheck]
# -Build rebuilds the tablet APK and padreen.exe first.  The staging folder dist\Padreen is produced by make-dist.ps1 (which also proves that
# padreen.exe can fetch the virtual display driver from Parsec; -SkipDriverCheck skips that when there is no network - never for a release).
param([switch]$Build, [switch]$SkipDriverCheck)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'      # not $PSHOME: that is pwsh's folder when this runs in PowerShell 7
$iscc = Find-Iscc
if (-not $iscc) { throw 'Inno Setup 7 not found: run tools\setup-build-env.ps1 (it installs it to D:\Tools\InnoSetup), or set PADREEN_ISCC to ISCC.exe' }
$args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$root\tools\make-dist.ps1")
if ($Build) { $args += '-Build' }
if ($SkipDriverCheck) { $args += '-SkipDriverCheck' }
& $ps @args
if ($LASTEXITCODE -ne 0) { throw 'make-dist failed' }
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
& $iscc "$root\installer\Padreen.iss"
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }
Get-ChildItem "$root\dist\Padreen-Setup-*.exe" | ForEach-Object { Write-Host ("installer: {0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB)) -ForegroundColor Green }