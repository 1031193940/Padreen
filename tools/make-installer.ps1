# Build the Windows installer:  dist\Padreen-Setup-<version>.exe  (Inno Setup 7 on D:\Tools\InnoSetup)
#   powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-Build]
# -Build rebuilds the tablet APK and padreen.exe first.  The staging folder dist\Padreen is produced by make-dist.ps1.
param([switch]$Build)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ps = "$PSHOME\powershell.exe"
$iscc = 'D:\Tools\InnoSetup\ISCC.exe'
if (-not (Test-Path $iscc)) { throw "Inno Setup not found at $iscc (see docs/STATE.md: install innosetup-7.1.1-x64.exe with /CURRENTUSER /DIR=D:\Tools\InnoSetup)" }
$args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$root\tools\make-dist.ps1")
if ($Build) { $args += '-Build' }
& $ps @args
if ($LASTEXITCODE -ne 0) { throw 'make-dist failed' }
$env:TEMP = Join-Path $root '.tmp'; $env:TMP = $env:TEMP
& $iscc "$root\installer\Padreen.iss"
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }
Get-ChildItem "$root\dist\Padreen-Setup-*.exe" | ForEach-Object { Write-Host ("installer: {0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB)) -ForegroundColor Green }