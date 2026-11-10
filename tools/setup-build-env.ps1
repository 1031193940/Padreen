# Sets up this PC to build and release Padreen.  What it installs goes below the tools folder (D:\Tools, or $env:PADREEN_TOOLS) and needs no
# administrator rights.  Every download is checked before it is used: against the SHA-256 pinned below (taken from the publisher's own release
# page / checksum file), programs also against their Authenticode signature.  Run it again any time: it only installs what is missing.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup-build-env.ps1 [-CheckOnly]
#
#   C++ compiler + Windows SDK   checked only: install the "Desktop development with C++" workload with the Visual Studio Installer
#   JDK 11                       Microsoft Build of OpenJDK 11 (zip)            -> <tools>\jdk-<version>
#   Android SDK                  must be there already (Android Studio, ANDROID_HOME); adds build-tools 32.0.0 / platform android-33 with its
#                                sdkmanager.  It does NOT accept any license for you: when sdkmanager asks for one it stops - run
#                                  sdkmanager --licenses  yourself.
#   Inno Setup 7                 builds the setup program                       -> <tools>\InnoSetup
#   GitHub CLI                   publishes a release from the command line      -> <tools>\gh      (log in once afterwards:  gh auth login)
#   7-Zip                        optional, checked only: just the development helper tools\fetch-vdd.ps1 needs it (padreen.exe unpacks Parsec's installers by itself)
# Nothing is put on PATH and no environment variable is set: the build scripts find the tools themselves (tools\toolchain.ps1).
# -CheckOnly   only report what is there and what is missing
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)

# Pinned downloads.  A newer version means new values here (JDK: https://learn.microsoft.com/java/openjdk/download, the .sha256sum.txt next to the
# file; Inno Setup: the asset digest on https://github.com/jrsoftware/issrc/releases; GitHub CLI: the checksums file of the release).
$jdkVersion = '11.0.33'
$jdkUrl = "https://aka.ms/download-jdk/microsoft-jdk-$jdkVersion-windows-x64.zip"
$jdkSha = '632FFD60FAEDB4047EADECB02B2F281E99E4C35E1E7711E5DCDC4C73E2185995'
$innoVersion = '7.1.1'
$innoUrl = "https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-$innoVersion-x64.exe"
$innoSha = '8A5A6711C3766A6E5FB74F280BA0E3551E719CE94538D901FDBD1F8F9920B3F0'
$innoSigner = 'Pyrsys B.V.'
$ghVersion = '2.103.3'
$ghUrl = "https://github.com/cli/cli/releases/download/v$ghVersion/gh_${ghVersion}_windows_amd64.zip"
$ghSha = '98D0CFA794721665D7973DD728C78AFF7BD836D58F10F6D7A03568ECCB0A22E0'

$tools = Get-ToolsDir
$dl = Join-Path $tools '_download'
$results = New-Object System.Collections.ArrayList

function Report([string]$tool, [string]$state, [string]$where) {
    [void]$results.Add([pscustomobject]@{ Tool = $tool; State = $state; Where = $where })
    $color = 'Green'
    if ($state -eq 'MISSING') { $color = 'Yellow' } elseif ($state -eq 'FAILED') { $color = 'Red' }
    Write-Host ('{0,-30} {1,-10} {2}' -f $tool, $state, $where) -ForegroundColor $color
}

# runs one part; a failure is reported and the next part still runs
function Step([string]$name, [scriptblock]$body) {
    try { & $body } catch { Report $name 'FAILED' ($_.Exception.Message -replace '\s+', ' ') }
}

# native programs write progress to stderr, which a script with $ErrorActionPreference = 'Stop' takes for an error
function Invoke-Quiet([scriptblock]$body) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $body } finally { $ErrorActionPreference = $old }
}

function Save-Verified([string]$url, [string]$file, [string]$sha256) {
    if (Test-Path -LiteralPath $file) {
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $sha256) { return }
        Remove-Item -LiteralPath $file -Force
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $file) | Out-Null
    Write-Host "  download $url"
    Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
    $h = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($h -ne $sha256) {
        Remove-Item -LiteralPath $file -Force
        throw "the download of $url has sha256 $h, expected ${sha256}: it was NOT used"
    }
}

function Expand-ZipTo([string]$zip, [string]$dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
}

# ISCC.exe has no version resource; the uninstaller that sits next to it carries the version of the Inno Setup that installed it
function Get-InnoVersion([string]$iscc) {
    $u = Join-Path (Split-Path -Parent $iscc) 'unins000.exe'
    if (Test-Path -LiteralPath $u) { return ([string](Get-Item -LiteralPath $u).VersionInfo.ProductVersion).Trim() }
    return 'unknown'
}

Write-Host "tools folder: $tools$(if ($CheckOnly) { '   (check only: nothing is installed)' })"
Write-Host ''

# ---------------------------------------------------------------------------------------------------------------- C++
Step 'C++ compiler + Windows SDK' {
    $what = Use-MsvcEnvironment
    $inc = $env:INCLUDE -split ';' | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'windows.h')) } | Select-Object -First 1
    if (-not $inc) { throw 'the Windows SDK (windows.h) is missing: add "Windows 11 SDK" to the Visual Studio workload' }
    Report 'C++ compiler + Windows SDK' 'ok' $what
}

# ---------------------------------------------------------------------------------------------------------------- JDK 11
Step "JDK $PadreenJdkMajor" {
    $jdk = Find-Jdk $PadreenJdkMajor
    if ($jdk) { Report "JDK $PadreenJdkMajor" 'ok' $jdk; return }
    $dest = Join-Path $tools "jdk-$jdkVersion"
    if ($CheckOnly) { Report "JDK $PadreenJdkMajor" 'MISSING' "would be installed to $dest"; return }
    if (Test-Path -LiteralPath $dest) { throw "$dest exists but is not a usable JDK ${PadreenJdkMajor}: remove that folder and run this again" }
    $zip = Join-Path $dl "microsoft-jdk-$jdkVersion-windows-x64.zip"
    Save-Verified $jdkUrl $zip $jdkSha
    $tmp = Join-Path $dl 'jdk-extract'
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    Expand-ZipTo $zip $tmp
    $inner = Get-ChildItem -LiteralPath $tmp -Directory | Select-Object -First 1
    if (-not $inner) { throw 'the JDK archive is empty' }
    Move-Item -LiteralPath $inner.FullName -Destination $dest
    Remove-Item -LiteralPath $tmp -Recurse -Force
    if (-not (Test-JdkHome $dest) -or (Get-JdkMajor $dest) -ne $PadreenJdkMajor) { throw "$dest is not a JDK $PadreenJdkMajor" }
    $sig = Get-AuthenticodeSignature -LiteralPath "$dest\bin\java.exe"
    Report "JDK $PadreenJdkMajor" 'installed' "$dest (java.exe signature: $($sig.Status))"
}

# ---------------------------------------------------------------------------------------------------------------- Android SDK
Step 'Android SDK' {
    $sdk = Find-AndroidSdk
    if (-not $sdk) { Report 'Android SDK' 'MISSING' 'install Android Studio (or the command-line tools) and set ANDROID_HOME'; return }
    $need = @()
    if (-not (Test-Path -LiteralPath "$sdk\build-tools\$PadreenBuildTools\aapt2.exe")) { $need += "build-tools;$PadreenBuildTools" }
    if (-not (Test-Path -LiteralPath "$sdk\platforms\$PadreenAndroidPlatform\android.jar")) { $need += "platforms;$PadreenAndroidPlatform" }
    if (-not (Test-Path -LiteralPath "$sdk\platform-tools\adb.exe")) { $need += 'platform-tools' }
    if (-not $need.Count) { Report 'Android SDK' 'ok' "$sdk (build-tools $PadreenBuildTools, $PadreenAndroidPlatform, platform-tools)"; return }
    if ($CheckOnly) { Report 'Android SDK' 'MISSING' ("$sdk lacks: " + ($need -join ', ')); return }
    $sm = Find-SdkManager $sdk
    if (-not $sm) { throw "sdkmanager not found below $sdk (install the Android command-line tools)" }
    $j = Find-JdkAtLeast 17
    if (-not $j) { throw 'the Android command-line tools need a JDK 17 or newer (Android Studio brings one); none was found' }
    $env:JAVA_HOME = $j
    Write-Host "  sdkmanager (with the JDK at $j) installs: $($need -join ', ')"
    # stdin gets an empty line: a license question is answered with "no" (nothing is accepted on your behalf)
    $log = Invoke-Quiet { '' | & $sm "--sdk_root=$sdk" --install @need 2>&1 | ForEach-Object { "$_" } }
    $still = @()
    if (-not (Test-Path -LiteralPath "$sdk\build-tools\$PadreenBuildTools\aapt2.exe")) { $still += "build-tools;$PadreenBuildTools" }
    if (-not (Test-Path -LiteralPath "$sdk\platforms\$PadreenAndroidPlatform\android.jar")) { $still += "platforms;$PadreenAndroidPlatform" }
    if (-not (Test-Path -LiteralPath "$sdk\platform-tools\adb.exe")) { $still += 'platform-tools' }
    if ($still.Count) {
        $tail = (($log | Where-Object { $_.Trim() } | Select-Object -Last 4) -join ' | ')
        throw ("sdkmanager did not install " + ($still -join ', ') + ". If it asked for a license, run  `"$sm`" --licenses  yourself. sdkmanager said: $tail")
    }
    Report 'Android SDK' 'installed' "${sdk}: $($need -join ', ')"
}

# ---------------------------------------------------------------------------------------------------------------- Inno Setup 7
Step 'Inno Setup 7' {
    $iscc = Find-Iscc
    if ($iscc) { Report 'Inno Setup 7' 'ok' "$iscc (version $(Get-InnoVersion $iscc))"; return }
    $dir = Join-Path $tools 'InnoSetup'
    if ($CheckOnly) { Report 'Inno Setup 7' 'MISSING' "would be installed to $dir"; return }
    $exe = Join-Path $dl "innosetup-$innoVersion-x64.exe"
    Save-Verified $innoUrl $exe $innoSha
    $sig = Get-AuthenticodeSignature -LiteralPath $exe
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notlike "*$innoSigner*") { throw "the Inno Setup installer is not validly signed by $innoSigner (status $($sig.Status))" }
    $log = Join-Path $dl 'innosetup-install.log'
    Write-Host "  installing Inno Setup $innoVersion to $dir (silent, current user only, no shortcuts, no file associations)"
    $argList = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', "/DIR=`"$dir`"", '/NOICONS', '/TASKS=""', "/LOG=`"$log`"")
    $p = Start-Process -FilePath $exe -ArgumentList $argList -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "the Inno Setup installer returned $($p.ExitCode) (log: $log)" }
    $iscc = Join-Path $dir 'ISCC.exe'
    if (-not (Test-Path -LiteralPath $iscc)) { throw "ISCC.exe is not at $iscc after the installation (log: $log)" }
    $lang = Join-Path $dir 'Languages\ChineseSimplified.isl'
    if (-not (Test-Path -LiteralPath $lang)) { throw "this Inno Setup has no Languages\ChineseSimplified.isl, which installer\Padreen.iss uses" }
    Report 'Inno Setup 7' 'installed' "$dir (version $(Get-InnoVersion $iscc), signature $($sig.Status))"
}

# ---------------------------------------------------------------------------------------------------------------- GitHub CLI
Step 'GitHub CLI' {
    $gh = Find-Gh
    if ($gh) { Report 'GitHub CLI' 'ok' $gh; return }
    $dest = Join-Path $tools 'gh'
    if ($CheckOnly) { Report 'GitHub CLI' 'MISSING' "would be installed to $dest"; return }
    if (Test-Path -LiteralPath $dest) { throw "$dest exists but has no bin\gh.exe: remove that folder and run this again" }
    $zip = Join-Path $dl "gh_${ghVersion}_windows_amd64.zip"
    Save-Verified $ghUrl $zip $ghSha
    $tmp = Join-Path $dl 'gh-extract'
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    Expand-ZipTo $zip $tmp
    if (-not (Test-Path -LiteralPath "$tmp\bin\gh.exe")) { throw 'bin\gh.exe is not in the GitHub CLI archive' }
    Move-Item -LiteralPath $tmp -Destination $dest
    $sig = Get-AuthenticodeSignature -LiteralPath "$dest\bin\gh.exe"
    Report 'GitHub CLI' 'installed' "$dest\bin\gh.exe (signature: $($sig.Status)); log in once with: gh auth login"
}

# ---------------------------------------------------------------------------------------------------------------- 7-Zip
Step '7-Zip' {
    $z = Find-SevenZip
    if ($z) { Report '7-Zip' 'ok' $z } else { Report '7-Zip' 'optional' 'not needed for a release; only the development helper tools\fetch-vdd.ps1 uses it (https://www.7-zip.org/)' }
}

if ((-not $CheckOnly) -and (Test-Path -LiteralPath $dl)) { Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host ''
$bad = @($results | Where-Object { $_.State -eq 'MISSING' -or $_.State -eq 'FAILED' })
if ($bad.Count) {
    Write-Host "$($bad.Count) item(s) need attention." -ForegroundColor Yellow
    exit 1
}
Write-Host 'The build environment is complete.' -ForegroundColor Green
