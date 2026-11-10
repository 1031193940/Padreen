# Development helper (NOT part of a release): assembles the virtual display driver package in tools\vdd\pkg:
#     tools\vdd\pkg\nefconw.exe  +  tools\vdd\pkg\driver\mm.inf, mm.dll, mm.cat
# for the PowerShell scripts next to it (install-vdd.ps1 ...).  padreen.exe does the same by itself at run time (it downloads the installers and
# unpacks them without 7-Zip), and no package ever contains Parsec's files (no permission to redistribute them has been found) - see tools\vdd\pkg\NOTICE.txt.
# The files are Parsec's, unchanged, taken from Parsec's own installers (builds.parsec.app); they are not kept in git.
# This script downloads the installers, checks every download and every file against the SHA-256 values below and for a valid signature, and
# unpacks them (the installers are NSIS archives: 7-Zip is needed).  Nothing is installed or run.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-vdd.ps1 [-Dest <folder>] [-PkgDir <folder>] [-SevenZip <7z.exe>] [-Force]
#   -Dest      also copy the finished package (binaries + scripts + NOTICE.txt) into this folder
#   -PkgDir    where the package is kept (default: tools\vdd\pkg)
#   -SevenZip  the 7z.exe to use when none is found by itself
#   -Force     download and unpack again even if the package is complete
# The same SHA-256 values are built into padreen.exe (host\padreen\src\driver.cpp): it refuses to use a package that differs.  When the driver
# version changes, change both places (padreen.exe --verify-driver --package <folder> checks a package against the exe's values).
param(
    [string]$Dest = '',
    [string]$PkgDir = '',
    [string]$SevenZip = '',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)     # Find-SevenZip
$gitPkg = Join-Path $root 'tools\vdd\pkg'          # scripts and NOTICE.txt live here (in git); the binaries are put here too unless -PkgDir says otherwise
if (-not $PkgDir) { $PkgDir = $gitPkg }

# Parsec's official downloads.  0.41 is the driver Padreen is built and tested with; nefconw.exe (nefcon 1.10.0, signed by Parsec) is only in the 0.45 installer.
$installers = [ordered]@{
    '0.41' = @{ Url = 'https://builds.parsec.app/vdd/parsec-vdd-0.41.0.0.exe'; Sha256 = '317977C5ABD58F1C3F7B27EF7F96947EFD07E6D35B27E415CEA5DAA6007629B1' }
    '0.45' = @{ Url = 'https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe'; Sha256 = 'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362' }
}
# The package: file -> installer it comes from, its SHA-256, the signer that has to be on it ('' = the .inf itself carries no signature; the signed .cat covers it).
$files = @(
    @{ Rel = 'nefconw.exe';   From = '0.45'; Sha256 = 'CF746D1B0BBB713993D4A90DCCD774C78D9FFF8C2BA5A054B6C8F56C77E1EEE1'; Signer = 'Parsec Cloud, Inc.' }
    @{ Rel = 'driver\mm.inf'; From = '0.41'; Sha256 = 'B003D2FE10C366FB454324149B46D6B14FB17ABB75F79FA8BC9E0043FC56C942'; Signer = '' }
    @{ Rel = 'driver\mm.dll'; From = '0.41'; Sha256 = '7F2E8860D56461EA5EB2D2D54A63FC934F3694B32EDA72170E069797D846D6C7'; Signer = 'Microsoft Windows Hardware Compatibility Publisher' }
    @{ Rel = 'driver\mm.cat'; From = '0.41'; Sha256 = '5852D0C23EC74A2F5594BC498525E7BEB29CE25BD7ADAC4906DDBDCBF539B1F9'; Signer = 'Microsoft Windows Hardware Compatibility Publisher' }
)
# kept in git next to the binaries
$tracked = @('install-vdd.ps1', 'uninstall-vdd.ps1', 'status-vdd.ps1', 'NOTICE.txt')

# $null = fine, otherwise the reason
function Test-Signed([string]$path, [string]$signer) {
    $s = Get-AuthenticodeSignature -LiteralPath $path
    if ($s.Status -ne 'Valid') { return "the signature is '$($s.Status)'" }
    if ($signer -and ($s.SignerCertificate.Subject -notlike "*$signer*")) { return "signed by '$($s.SignerCertificate.Subject)', expected '$signer'" }
    return $null
}

# $null = the four binaries in $dir are all there and exactly the expected ones, otherwise the reason
function Test-Package([string]$dir) {
    foreach ($f in $files) {
        $p = Join-Path $dir $f.Rel
        if (-not (Test-Path -LiteralPath $p)) { return "missing: $($f.Rel)" }
        $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
        if ($h -ne $f.Sha256) { return "$($f.Rel) is not the expected file (sha256 $h)" }
        if ($f.Signer -ne '') {
            $bad = Test-Signed $p $f.Signer
            if ($bad) { return "$($f.Rel): $bad" }
        }
    }
    return $null
}

function Get-Installer([string]$ver, [string]$dir) {
    $i = $installers[$ver]
    $exe = Join-Path $dir "parsec-vdd-$ver.exe"
    if (-not (Test-Path -LiteralPath $exe) -or ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $i.Sha256)) {
        Write-Host "download $($i.Url)"
        Invoke-WebRequest -Uri $i.Url -OutFile $exe -UseBasicParsing
    }
    $h = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if ($h -ne $i.Sha256) {
        Remove-Item -LiteralPath $exe -Force
        throw "parsec-vdd-$ver.exe has sha256 $h, expected $($i.Sha256): Parsec changed the file? It is not used."
    }
    $bad = Test-Signed $exe 'Parsec Cloud, Inc.'
    if ($bad) { throw "parsec-vdd-${ver}.exe: $bad" }
    return $exe
}

$why = Test-Package $PkgDir
if ($Force -or $why) {
    if ($why) { Write-Host "driver package in $PkgDir is not usable ($why): fetching it from Parsec" }
    $z = if ($SevenZip -and (Test-Path -LiteralPath $SevenZip)) { $SevenZip } else { Find-SevenZip }
    if (-not $z) {
        $list = ($files | ForEach-Object { "  {0}  (sha256 {1}, from parsec-vdd-{2}.0.0.exe)" -f $_.Rel, $_.Sha256, $_.From }) -join "`n"
        throw ("7-Zip was not found (Parsec's installers are NSIS archives that only 7-Zip unpacks). Install it from https://www.7-zip.org/ or pass -SevenZip <path to 7z.exe>.`n" +
               "Or unpack the installers yourself and put these files into ${PkgDir}:`n$list")
    }
    $work = Join-Path $root '.tmp\vdd-fetch'
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        foreach ($ver in @($files | ForEach-Object { $_.From } | Select-Object -Unique)) {
            $exe = Get-Installer $ver $work
            $x = Join-Path $work "x$ver"
            & $z x $exe "-o$x" -y | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "7-Zip could not unpack $exe (exit code $LASTEXITCODE)" }
        }
        foreach ($f in $files) {
            $dst = Join-Path $PkgDir $f.Rel
            New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
            Copy-Item -LiteralPath (Join-Path (Join-Path $work "x$($f.From)") $f.Rel) -Destination $dst -Force
        }
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    $why = Test-Package $PkgDir
    if ($why) { throw "the unpacked driver package is not what was expected: $why" }
    Write-Host "driver package ready: $PkgDir"
} else {
    Write-Host "driver package is complete: $PkgDir"
}

if ($Dest) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    foreach ($f in $files) {
        $d = Join-Path $Dest $f.Rel
        New-Item -ItemType Directory -Force (Split-Path -Parent $d) | Out-Null
        Copy-Item -LiteralPath (Join-Path $PkgDir $f.Rel) -Destination $d -Force
    }
    foreach ($t in $tracked) { Copy-Item -LiteralPath (Join-Path $gitPkg $t) -Destination (Join-Path $Dest $t) -Force }
    $why = Test-Package $Dest
    if ($why) { throw "the copy in $Dest is not right: $why" }
    Write-Host "driver package copied to $Dest"
}
