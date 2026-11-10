# Build Padreen (MSVC 14.34 on D: + NuGet Windows SDK in third_party).  Output: host\padreen\build\padreen.exe
#   powershell -ExecutionPolicy Bypass -File host\padreen\build.ps1 [-Debug]
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Invoke-Expression (Get-Content "$here\vcenv.ps1" -Raw) | Out-Null
$out = "$here\build"
New-Item -ItemType Directory -Force $out | Out-Null
$root = Split-Path -Parent (Split-Path -Parent $here)
$opt = if ($Debug) { @('/Od', '/Zi', '/MDd') } else { @('/O2', '/MD', '/DNDEBUG') }
$srcs = Get-ChildItem "$here\src\*.cpp" | ForEach-Object { $_.FullName }
$srcs += "$root\third_party\lz4\lz4.c"
# resources: icons, manifest (Per-Monitor DPI v2, common controls 6), version info
& rc.exe /nologo /c65001 /I"$here\res" "/fo$out\padreen.res" "$here\res\padreen.rc"
if ($LASTEXITCODE -ne 0) { Write-Host "RC FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
$srcs += "$out\padreen.res"
$cl = @('/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W3', '/DUNICODE', '/D_UNICODE', '/D_CRT_SECURE_NO_WARNINGS', "/Fo$out\") + $opt + $srcs +
      @("/Fe$out\padreen.exe", '/link', '/SUBSYSTEM:WINDOWS', "/MAP:$out\padreen.map", 'd3d11.lib', 'dxgi.lib', 'ws2_32.lib', 'setupapi.lib', 'shell32.lib', 'user32.lib', 'gdi32.lib', 'avrt.lib', 'advapi32.lib', 'ole32.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'comctl32.lib', 'powrprof.lib', 'shcore.lib')
& cl.exe @cl 2>&1 | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
Copy-Item "$here\assets\padreen-white.png" "$out\padreen-white.png" -Force
Write-Host "built: $out\padreen.exe" -ForegroundColor Green
