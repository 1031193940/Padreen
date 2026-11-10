# Sets INCLUDE / LIB / PATH (and TEMP) for the MSVC x64 tools cl / rc / link; Use-MsvcEnvironment in tools\toolchain.ps1 does the work (any Visual
# Studio with the C++ workload, or the original development PC's MSVC 14.34 + NuGet Windows SDK layout).  Needs $root = the repository folder.
# build.ps1 loads it like this (this works under any execution policy; dot-sourcing does not):
#     $root = '<repository folder>';  Invoke-Expression (Get-Content "$root\host\padreen\vcenv.ps1" -Raw)
Invoke-Expression (Get-Content "$root\tools\toolchain.ps1" -Raw)
Use-MsvcEnvironment
