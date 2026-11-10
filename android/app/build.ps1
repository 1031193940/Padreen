# Build the Padreen APK without Gradle (aapt2 + javac + d8 + zipalign + apksigner).
# Everything (temp files, build output, signing key) stays on D:.
param([switch]$Install, [switch]$Start)
$ErrorActionPreference = 'Stop'

$proj   = 'D:\Workspace\MatePadPaper'
$root   = $PSScriptRoot
$sdk    = 'D:\Android\android-sdk'
$bt     = "$sdk\build-tools\32.0.0"
$jar    = "$sdk\platforms\android-33\android.jar"
$jdk    = 'C:\Program Files\Microsoft\jdk-11.0.12.7-hotspot'   # pre-existing JDK 11
$adb    = "$sdk\platform-tools\adb.exe"
$out    = "$root\build"
$ks     = "$proj\tools\keys\dev.jks"
$pkg    = 'com.padreen.app'

# keep every temp file off C:
$env:TEMP = "$proj\.tmp"; $env:TMP = $env:TEMP
$env:JAVA_HOME = $jdk
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

if (-not (Test-Path $ks)) {
    New-Item -ItemType Directory -Force (Split-Path $ks) | Out-Null
    & "$jdk\bin\keytool.exe" -genkeypair -keystore $ks -storepass paperdisplay -keypass paperdisplay `
        -alias dev -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Padreen Dev' | Out-Null
}

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\classes", "$out\dex" | Out-Null

$src = (Get-ChildItem "$root\src" -Recurse -Filter *.java).FullName
& "$jdk\bin\javac.exe" -source 8 -target 8 -encoding UTF-8 -Xlint:-options -bootclasspath $jar -d "$out\classes" @src
if ($LASTEXITCODE) { throw 'javac failed' }

$java = "$jdk\bin\java.exe"
$cls = (Get-ChildItem "$out\classes" -Recurse -Filter *.class).FullName
# d8.bat passes -Djava.ext.dirs, which JDK 9+ rejects -> run the jar directly
& $java -cp "$bt\lib\d8.jar" com.android.tools.r8.D8 --lib $jar --min-api 29 --output "$out\dex" @cls
if ($LASTEXITCODE) { throw 'd8 failed' }

# resources (launcher icon: res/mipmap-*, adaptive icon)
& "$bt\aapt2.exe" compile --dir "$root\res" -o "$out\res.zip"
if ($LASTEXITCODE) { throw 'aapt2 compile failed' }
& "$bt\aapt2.exe" link -o "$out\unsigned.apk" -I $jar --manifest "$root\AndroidManifest.xml" `
    --min-sdk-version 29 --target-sdk-version 29 --version-code 1 --version-name 0.1 "$out\res.zip"
if ($LASTEXITCODE) { throw 'aapt2 failed' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open("$out\unsigned.apk", 'Update')
[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$out\dex\classes.dex", 'classes.dex', 'Optimal') | Out-Null
$zip.Dispose()

& "$bt\zipalign.exe" -f -p 4 "$out\unsigned.apk" "$out\aligned.apk"
if ($LASTEXITCODE) { throw 'zipalign failed' }
& $java -jar "$bt\lib\apksigner.jar" sign --ks $ks --ks-pass pass:paperdisplay --key-pass pass:paperdisplay --out "$out\padreen.apk" "$out\aligned.apk"
if ($LASTEXITCODE) { throw 'apksigner failed' }
"built: $out\padreen.apk ({0:N0} bytes)" -f (Get-Item "$out\padreen.apk").Length

if ($Install) {
    & $adb install -r "$out\padreen.apk"
    if ($LASTEXITCODE) { throw 'adb install failed' }
}
if ($Start) {
    & $adb shell am start -n "$pkg/.MainActivity"
}

