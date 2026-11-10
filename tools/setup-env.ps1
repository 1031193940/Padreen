# One-shot environment setup for the MatePad Paper display project.
# Every download, cache and temp file stays on D:. Nothing here needs administrator rights.
#   powershell -NoProfile -ExecutionPolicy Bypass -File setup-env.ps1 [-Ndk] [-WinSdk] [-Lz4] [-Vdd]
param([switch]$Ndk, [switch]$WinSdk, [switch]$Lz4, [switch]$Vdd)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$proj = 'D:\Workspace\MatePadPaper'
$env:TEMP = "$proj\.tmp"; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP, "$proj\docs", "$proj\third_party", "$proj\tools" | Out-Null
$logFile = "$proj\docs\setup-log.txt"
function Log($m) { $l = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $m; Write-Host $l; Add-Content -Path $logFile -Value $l }

function Download($url, $dest) {
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Log "download $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    $h = (Get-FileHash $dest -Algorithm SHA256).Hash
    Log ("  -> {0} ({1:N0} bytes) sha256={2}" -f $dest, (Get-Item $dest).Length, $h)
}

if ($Ndk) {
    $env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-11.0.12.7-hotspot'
    $env:ANDROID_USER_HOME = "$proj\.cache\android-user"     # sdkmanager cache only; adb is not run under this variable
    New-Item -ItemType Directory -Force $env:ANDROID_USER_HOME | Out-Null
    $sm = 'D:\Android\android-sdk\cmdline-tools\7.0\bin\sdkmanager.bat'
    Log 'accepting Android SDK licenses (needed to install the NDK)'
    1..40 | ForEach-Object { 'y' } | & $sm --sdk_root=D:\Android\android-sdk --licenses | Out-Null
    Log 'installing ndk;27.3.13750724 into D:\Android\android-sdk\ndk'
    & $sm --sdk_root=D:\Android\android-sdk 'ndk;27.3.13750724' 2>&1 | ForEach-Object { Log "  sdkmanager: $_" }
    Log ('NDK present: ' + (Test-Path 'D:\Android\android-sdk\ndk\27.3.13750724\toolchains'))
}

if ($WinSdk) {
    # Official Microsoft NuGet packages -> headers + import libs for the MSVC that ships with VS 2022 (no system install)
    $ver = '10.0.26100.9457'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($id in 'Microsoft.Windows.SDK.CPP', 'Microsoft.Windows.SDK.CPP.x64') {
        $lid = $id.ToLower()
        $pkg = "$proj\third_party\nupkg\$id.$ver.nupkg"
        if (-not (Test-Path $pkg)) { Download "https://api.nuget.org/v3-flatcontainer/$lid/$ver/$lid.$ver.nupkg" $pkg }
        $dst = "$proj\third_party\winsdk\$id"
        if (-not (Test-Path $dst)) {
            Log "extract $id -> $dst"
            [IO.Compression.ZipFile]::ExtractToDirectory($pkg, $dst)
        }
    }
    Log 'Windows SDK packages ready'
}

if ($Lz4) {
    $tag = 'v1.10.0'
    foreach ($f in 'lz4.c', 'lz4.h', 'LICENSE') {
        Download "https://raw.githubusercontent.com/lz4/lz4/$tag/lib/$f" "$proj\third_party\lz4\$f"
    }
    Log "LZ4 $tag ready"
}

if ($Vdd) {
    foreach ($v in '0.41.0.0', '0.45.0.0') {
        Download "https://builds.parsec.app/vdd/parsec-vdd-$v.exe" "$proj\tools\vdd\parsec-vdd-$v.exe"
    }
    Log 'Parsec VDD installers downloaded (NOT executed)'
}
Log 'setup-env done'
