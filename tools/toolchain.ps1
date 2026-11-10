# Where the build tools are on this PC.  Used by the build / packaging scripts and by setup-build-env.ps1.  Load it with
#     Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)
# ($root = the repository folder.  That works under any execution policy; dot-sourcing a script does not.)
# Every Find-* function returns the first candidate that exists (or $null): the environment variable that names the tool, the folder
# tools\setup-build-env.ps1 installs it into, the layout of the original development PC, the usual installation folders.
# Nothing here changes the system (no registry, no PATH); Use-MsvcEnvironment only changes the environment of the calling process.
#   PADREEN_TOOLS      where setup-build-env.ps1 installs things (default D:\Tools)
#   PADREEN_JDK        JDK home to build the APK with     PADREEN_ISCC   ISCC.exe of Inno Setup 7     PADREEN_GH   gh.exe
#   PADREEN_KEYSTORE / PADREEN_KEYSTORE_PASS   APK signing key (see android\app\build.ps1)

# What a release is built with: the versions padreen.exe and the tablet app were developed and tested against.
$PadreenJdkMajor = 11
$PadreenBuildTools = '32.0.0'
$PadreenAndroidPlatform = 'android-33'

# Where setup-build-env.ps1 installs what it installs (no administrator rights needed): D:\Tools when there is a D: drive (the project's rule:
# software goes to D:, not C:), else %LOCALAPPDATA%\Padreen\tools.
function Get-ToolsDir {
    if ($env:PADREEN_TOOLS) { return $env:PADREEN_TOOLS }
    if (Test-Path -LiteralPath 'D:\') { return 'D:\Tools' }
    return (Join-Path $env:LOCALAPPDATA 'Padreen\tools')
}

function Find-FirstExisting([object[]]$paths) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath ([string]$p))) { return (Resolve-Path -LiteralPath ([string]$p)).ProviderPath }
    }
    return $null
}

# 11.0.12.7 in "jdk-11.0.12.7-hotspot": folder names sort by version, not by text (jdk-11.0.33 before jdk-11.0.9)
function ConvertTo-SortVersion([string]$name) {
    $m = [regex]::Match($name, '\d+(\.\d+)*').Value
    if (-not $m) { return [version]'0.0' }
    $parts = @($m.Split('.'))
    while ($parts.Count -lt 2) { $parts += '0' }
    if ($parts.Count -gt 4) { $parts = $parts[0..3] }
    try { return [version]($parts -join '.') } catch { return [version]'0.0' }
}

function Find-VersionedDirs([string[]]$patterns) {
    $found = @()
    foreach ($pat in $patterns) { $found += @(Get-ChildItem -Path $pat -Directory -ErrorAction SilentlyContinue) }
    return @($found | Sort-Object -Property @{ Expression = { ConvertTo-SortVersion $_.Name }; Descending = $true } | ForEach-Object { $_.FullName })
}

# ---------------------------------------------------------------------------------------------------------------- Java
function Get-JdkMajor([string]$jdkHome) {
    $rel = Join-Path $jdkHome 'release'
    if (-not (Test-Path -LiteralPath $rel)) { return 0 }
    $line = Get-Content -LiteralPath $rel | Where-Object { $_ -match '^JAVA_VERSION=' } | Select-Object -First 1
    if (-not $line) { return 0 }
    $v = ($line -replace '^JAVA_VERSION=', '').Trim('"')
    $p = $v.Split('.')
    if ($p[0] -eq '1' -and $p.Count -gt 1) { return [int]($p[1] -replace '\D.*$', '') }     # 1.8.0_x = Java 8
    try { return [int]($p[0] -replace '\D.*$', '') } catch { return 0 }
}

function Test-JdkHome([string]$h) {
    return [bool]($h -and (Test-Path -LiteralPath "$h\bin\java.exe") -and (Test-Path -LiteralPath "$h\bin\javac.exe") -and (Test-Path -LiteralPath "$h\bin\keytool.exe"))
}

function Get-JdkSearchPatterns([string]$versionPattern) {
    $pats = @((Join-Path (Get-ToolsDir) "jdk-$versionPattern"))
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }) {
        $pats += "$pf\Microsoft\jdk-$versionPattern", "$pf\Eclipse Adoptium\jdk-$versionPattern", "$pf\Java\jdk-$versionPattern", "$pf\Java\jdk$versionPattern", "$pf\Amazon Corretto\jdk$versionPattern", "$pf\Zulu\zulu-$versionPattern"
    }
    return $pats
}

# A JDK (java, javac, keytool) of exactly this major version.  PADREEN_JDK is taken as it is.
function Find-Jdk([int]$major) {
    $cands = @()
    if ($env:PADREEN_JDK) { $cands += $env:PADREEN_JDK }
    $cands += Find-VersionedDirs (Get-JdkSearchPatterns "$major*")
    if ($env:JAVA_HOME) { $cands += $env:JAVA_HOME }
    foreach ($c in $cands) {
        if (-not (Test-JdkHome $c)) { continue }
        if ($c -eq $env:PADREEN_JDK -or (Get-JdkMajor $c) -eq $major) { return $c }
    }
    return $null
}

# A JDK of at least this major version (the Android command-line tools need 17): any JDK, or the one that comes with Android Studio.
function Find-JdkAtLeast([int]$min) {
    $cands = @()
    $cands += Find-VersionedDirs (Get-JdkSearchPatterns '*')
    if ($env:JAVA_HOME) { $cands += $env:JAVA_HOME }
    foreach ($as in @("$env:ProgramFiles\Android\Android Studio", 'D:\Android Studio', "$env:LOCALAPPDATA\Programs\Android Studio")) { $cands += "$as\jbr" }
    foreach ($c in $cands) { if ((Test-JdkHome $c) -and ((Get-JdkMajor $c) -ge $min)) { return $c } }
    return $null
}

# ---------------------------------------------------------------------------------------------------------------- Android SDK
function Find-AndroidSdk {
    return (Find-FirstExisting @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, 'D:\Android\android-sdk', 'D:\Android\SDK', (Join-Path $env:LOCALAPPDATA 'Android\Sdk')))
}

function Find-SdkManager([string]$sdk) {
    if (-not $sdk) { return $null }
    $cands = @("$sdk\cmdline-tools\latest\bin\sdkmanager.bat")
    $cands += @(Find-VersionedDirs @("$sdk\cmdline-tools\[0-9]*") | ForEach-Object { "$_\bin\sdkmanager.bat" })
    $cands += "$sdk\tools\bin\sdkmanager.bat"
    return (Find-FirstExisting $cands)
}

# the folder whose adb.exe + AdbWinApi.dll + AdbWinUsbApi.dll go into the release package
function Find-AdbDir {
    $sdk = Find-AndroidSdk
    if (-not $sdk) { return $null }
    $d = Join-Path $sdk 'platform-tools'
    if ((Test-Path -LiteralPath "$d\adb.exe") -and (Test-Path -LiteralPath "$d\AdbWinApi.dll") -and (Test-Path -LiteralPath "$d\AdbWinUsbApi.dll")) { return $d }
    return $null
}

# ---------------------------------------------------------------------------------------------------------------- packaging / publishing
# Inno Setup 7 (the setup program uses its Chinese language file).  Inno Setup 6 would not do.
function Find-Iscc {
    return (Find-FirstExisting @($env:PADREEN_ISCC, (Join-Path (Get-ToolsDir) 'InnoSetup\ISCC.exe'), "$env:ProgramFiles\Inno Setup 7\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe"))
}

# 7-Zip unpacks the NSIS installers of the virtual display driver (tools\fetch-vdd.ps1)
function Find-SevenZip {
    $c = @()
    if ($env:SEVENZIP) { $c += $env:SEVENZIP }
    $cmd = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($cmd) { $c += $cmd.Source }
    $c += "$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", 'D:\7-Zip\7z.exe', 'D:\Program Files\7-Zip\7z.exe'
    $c += @(Get-ChildItem 'D:\Unity\*\Editor\Data\Tools\7z.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })     # the one that comes with Unity
    return (Find-FirstExisting $c)
}

# GitHub CLI (publishing a release from the command line)
function Find-Gh {
    $p = Find-FirstExisting @($env:PADREEN_GH, (Join-Path (Get-ToolsDir) 'gh\bin\gh.exe'), "$env:ProgramFiles\GitHub CLI\gh.exe")
    if (-not $p) { $c = Get-Command gh.exe -ErrorAction SilentlyContinue; if ($c) { $p = $c.Source } }
    return $p
}

# ---------------------------------------------------------------------------------------------------------------- C++ (MSVC)
# Path of a Visual Studio (2022 or newer, Build Tools included) that has the C++ x64 compiler, or $null.
function Find-VisualStudio {
    $vw = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vw)) { return $null }
    $p = & $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
    if ($p -and (Test-Path -LiteralPath "$p\VC\Auxiliary\Build\vcvars64.bat")) { return [string]$p }
    return $null
}

# Puts the MSVC x64 tools (cl, rc, link), the headers and the libraries into INCLUDE / LIB / PATH of this process, and points TEMP at
# <repo>\.tmp (cl needs a temp folder that exists; the project keeps temp files off C:).  Returns a line that says what was found; throws
# when there is no compiler.
function Use-MsvcEnvironment([string]$repo = $root) {
    $tmp = Join-Path $repo '.tmp'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $env:TEMP = $tmp
    $env:TMP = $tmp
    $lz4 = "$repo\third_party\lz4"
    # 1) the original development PC: MSVC 14.34 on D: and the Windows SDK as NuGet packages kept in third_party (tools\setup-env.ps1 -WinSdk)
    $msvc = 'D:\Visual Studio Professional 2022\IDE\VC\Tools\MSVC\14.34.31933'
    $sdkv = '10.0.26100.0'
    $sdk = "$repo\third_party\winsdk\Microsoft.Windows.SDK.CPP\c"
    $sdkx = "$repo\third_party\winsdk\Microsoft.Windows.SDK.CPP.x64\c"
    if ((Test-Path -LiteralPath "$msvc\bin\Hostx64\x64\cl.exe") -and (Test-Path -LiteralPath "$sdk\Include\$sdkv\um\windows.h")) {
        $env:INCLUDE = @("$msvc\include", "$sdk\Include\$sdkv\ucrt", "$sdk\Include\$sdkv\um", "$sdk\Include\$sdkv\shared", "$sdk\Include\$sdkv\winrt", $lz4) -join ';'
        $env:LIB = @("$msvc\lib\x64", "$sdkx\ucrt\x64", "$sdkx\um\x64") -join ';'
        $env:PATH = @("$msvc\bin\Hostx64\x64", "$sdk\bin\$sdkv\x64", $env:PATH) -join ';'
        return "cl.exe: $msvc\bin\Hostx64\x64\cl.exe (MSVC 14.34, Windows SDK $sdkv from third_party)"
    }
    # 2) any Visual Studio with the C++ workload: its own vcvars64.bat sets everything (the Windows SDK it installed included)
    $vs = Find-VisualStudio
    if (-not $vs) { throw 'No Visual Studio with the "Desktop development with C++" workload was found: install it with the Visual Studio Installer (see tools\setup-build-env.ps1).' }
    $bat = Join-Path $tmp 'padreen-vcvars.cmd'
    Set-Content -LiteralPath $bat -Encoding ASCII -Value @('@echo off', "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1", 'if errorlevel 1 exit /b 1', 'set')
    $lines = & $bat
    $failed = ($LASTEXITCODE -ne 0)
    Remove-Item -LiteralPath $bat -Force -ErrorAction SilentlyContinue
    if ($failed) { throw "vcvars64.bat of $vs failed" }
    foreach ($l in $lines) {
        if ($l -match '^([^=]+)=(.*)$') { try { Set-Item -LiteralPath ('env:' + $Matches[1]) -Value $Matches[2] } catch { } }
    }
    $env:INCLUDE = "$env:INCLUDE;$lz4"
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { throw "cl.exe is not on the path after vcvars64.bat of $vs" }
    return "cl.exe: $($cl.Source) (Visual Studio at $vs)"
}
