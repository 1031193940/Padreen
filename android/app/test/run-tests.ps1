# JVM unit tests of the tablet's pure-logic classes (no device needed): the touch gate, the orientation negotiation, the volume-key rule and
# the accelerometer orientation detector.
#   powershell -ExecutionPolicy Bypass -File android\app\test\run-tests.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$jdk = 'C:\Program Files\Microsoft\jdk-11.0.12.7-hotspot\bin'
$out = Join-Path $here '..\build\test-classes'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$srcs = @(Get-ChildItem "$here\stubs" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$srcs += "$here\..\src\com\padreen\app\TouchGate.java"
$srcs += "$here\..\src\com\padreen\app\OrientSync.java"
$srcs += "$here\..\src\com\padreen\app\VolumeKeys.java"
$srcs += "$here\..\src\com\padreen\app\OrientationDetector.java"
$srcs += "$here\com\padreen\app\TouchGateTest.java"
$srcs += "$here\com\padreen\app\OrientSyncTest.java"
$srcs += "$here\com\padreen\app\VolumeKeysTest.java"
$srcs += "$here\com\padreen\app\OrientationDetectorTest.java"
& "$jdk\javac.exe" -encoding UTF-8 -d $out @srcs
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
$rc = 0
foreach ($t in 'com.padreen.app.TouchGateTest', 'com.padreen.app.OrientSyncTest', 'com.padreen.app.VolumeKeysTest', 'com.padreen.app.OrientationDetectorTest') {
    Write-Host "=== $t"
    & "$jdk\java.exe" -cp $out $t
    if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
}
exit $rc
