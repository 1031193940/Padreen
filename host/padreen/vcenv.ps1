# Sets INCLUDE / LIB / PATH for MSVC 14.34 (Visual Studio 2022 on D:) + the NuGet Windows SDK 10.0.26100 kept in third_party.
# Dot-source it:   . D:\Workspace\MatePadPaper\host\padreen\vcenv.ps1
$root = 'D:\Workspace\MatePadPaper'
$msvc = 'D:\Visual Studio Professional 2022\IDE\VC\Tools\MSVC\14.34.31933'
$sdkv = '10.0.26100.0'
$sdk  = "$root\third_party\winsdk\Microsoft.Windows.SDK.CPP\c"
$sdkx = "$root\third_party\winsdk\Microsoft.Windows.SDK.CPP.x64\c"
$env:INCLUDE = @("$msvc\include", "$sdk\Include\$sdkv\ucrt", "$sdk\Include\$sdkv\um", "$sdk\Include\$sdkv\shared", "$sdk\Include\$sdkv\winrt", "$root\third_party\lz4") -join ';'
$env:LIB     = @("$msvc\lib\x64", "$sdkx\ucrt\x64", "$sdkx\um\x64") -join ';'
$env:PATH    = @("$msvc\bin\Hostx64\x64", "$sdk\bin\$sdkv\x64", $env:PATH) -join ';'
$env:TEMP = "$root\.tmp"; $env:TMP = $env:TEMP
"cl.exe: " + (Get-Command cl.exe -ErrorAction SilentlyContinue).Source