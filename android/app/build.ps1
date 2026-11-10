# Build the Padreen APK without Gradle (aapt2 + javac + d8 + zipalign + apksigner).  Output: android\app\build\padreen.apk
#   powershell -NoProfile -ExecutionPolicy Bypass -File android\app\build.ps1 [-Install] [-Start] [-NewKey]
# Tools: JDK 11, Android SDK build-tools 32.0.0 and platform android-33 (tools\setup-build-env.ps1 installs what is missing and
# tools\toolchain.ps1 says where they are looked for).  Temp files stay in <repository>\.tmp, off C:.
#
# Signing key: tools\keys\release.jks, or the file named by $env:PADREEN_KEYSTORE.  The folder tools\keys is ignored by git and the key must NEVER
# be committed or uploaded: this repository is public, and a published key lets anyone sign "updates" that Android accepts for this app.
# Its password: $env:PADREEN_KEYSTORE_PASS, else the text in <keystore>.pass.  There is no default password.  The password reaches the signing
# tools through an environment variable of this process, never on a command line (where other programs, security agents included, can read it).
# The key is never created by itself: an app can only be updated by an app that is signed with the SAME key.  A build with another key cannot
# update the apps that are installed already - the tablets would have to uninstall first and pair again.  Create the release key ONCE, on
# purpose, with -NewKey, and keep a backup of tools\keys somewhere private: a lost key cannot be replaced.  (The original development key,
# CN=PaperDisplay Dev, whose password is public in this repository's history, is retired.)
param([switch]$Install, [switch]$Start, [switch]$NewKey)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot                                           # android\app
$proj = Split-Path -Parent (Split-Path -Parent $root)           # the repository
Invoke-Expression (Get-Content "$proj\tools\toolchain.ps1" -Raw)

$sdk = Find-AndroidSdk
$jdk = Find-Jdk $PadreenJdkMajor
if (-not $sdk) { throw 'Android SDK not found (set ANDROID_HOME; see tools\setup-build-env.ps1)' }
if (-not $jdk) { throw "JDK $PadreenJdkMajor not found (tools\setup-build-env.ps1 installs it; or set PADREEN_JDK)" }
$bt = "$sdk\build-tools\$PadreenBuildTools"
$jar = "$sdk\platforms\$PadreenAndroidPlatform\android.jar"
$adb = "$sdk\platform-tools\adb.exe"
foreach ($f in "$bt\aapt2.exe", "$bt\zipalign.exe", "$bt\lib\d8.jar", "$bt\lib\apksigner.jar", $jar) {
    if (-not (Test-Path -LiteralPath $f)) { throw "missing: $f (tools\setup-build-env.ps1 installs Android build-tools $PadreenBuildTools and platform $PadreenAndroidPlatform)" }
}
$out = "$root\build"
$pkg = 'com.padreen.app'
$java = "$jdk\bin\java.exe"
$ks = if ($env:PADREEN_KEYSTORE) { $env:PADREEN_KEYSTORE } else { "$proj\tools\keys\release.jks" }

# keep every temp file off C:
$env:TEMP = "$proj\.tmp"; $env:TMP = $env:TEMP
$env:JAVA_HOME = $jdk
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

function Get-KeystorePassword {
    if ($env:PADREEN_KEYSTORE_PASS) { return $env:PADREEN_KEYSTORE_PASS }
    if (Test-Path -LiteralPath "$ks.pass") { return (Get-Content -LiteralPath "$ks.pass" -Raw).Trim() }
    throw "no password for ${ks}: put it into the text file $ks.pass or into the environment variable PADREEN_KEYSTORE_PASS"
}

if ($NewKey) {
    if (Test-Path -LiteralPath $ks) { throw "$ks exists already and is not replaced (delete it yourself if you really want another key)" }
    $keyDir = Split-Path -Parent $ks
    New-Item -ItemType Directory -Force $keyDir | Out-Null
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    & icacls.exe $keyDir /inheritance:r /grant:r "${me}:(OI)(CI)F" /grant:r '*S-1-5-18:(OI)(CI)F' | Out-Null      # only this user (and SYSTEM) may read the key folder
    $bytes = New-Object byte[] 18
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    $rng.GetBytes($bytes)
    $rng.Dispose()
    $pw = [Convert]::ToBase64String($bytes) -replace '[+/=]', 'x'
    Set-Content -LiteralPath "$ks.pass" -Value $pw -Encoding ASCII -NoNewline
    $env:PADREEN_KS_PASS = $pw
    & "$jdk\bin\keytool.exe" -genkeypair -keystore $ks -storepass:env PADREEN_KS_PASS -keypass:env PADREEN_KS_PASS -alias release -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Padreen'
    if ($LASTEXITCODE) { throw 'keytool failed' }
    Write-Host "NEW signing key: $ks  (password in $ks.pass)" -ForegroundColor Yellow
    Write-Host 'Back BOTH files up somewhere private NOW (a password manager, an encrypted drive): an app can only be updated with the key it was first signed with.' -ForegroundColor Yellow
    Write-Host 'Never commit or upload them: this repository is public.' -ForegroundColor Yellow
} elseif (-not (Test-Path -LiteralPath $ks)) {
    throw ("signing key not found: $ks`n" +
           "Create the release key once, on purpose:  android\app\build.ps1 -NewKey   (an app can only be updated by an app signed with the SAME key, so keep a backup of tools\keys; it must never go into git).`n" +
           "Already have a key?  Put it at that path, or set PADREEN_KEYSTORE (and PADREEN_KEYSTORE_PASS, or a text file <keystore>.pass next to it).")
}

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\classes", "$out\dex" | Out-Null

$src = (Get-ChildItem "$root\src" -Recurse -Filter *.java).FullName
& "$jdk\bin\javac.exe" -source 8 -target 8 -encoding UTF-8 -Xlint:-options -bootclasspath $jar -d "$out\classes" @src
if ($LASTEXITCODE) { throw 'javac failed' }

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
$env:PADREEN_KS_PASS = Get-KeystorePassword
& $java -jar "$bt\lib\apksigner.jar" sign --ks $ks --ks-pass env:PADREEN_KS_PASS --key-pass env:PADREEN_KS_PASS --out "$out\padreen.apk" "$out\aligned.apk"
if ($LASTEXITCODE) { throw 'apksigner failed (wrong key password? PADREEN_KEYSTORE_PASS, or the text file <keystore>.pass)' }
"built: $out\padreen.apk ({0:N0} bytes)" -f (Get-Item "$out\padreen.apk").Length
# which key signed it: compare with the key of the apps that are installed (an update needs the same certificate)
& $java -jar "$bt\lib\apksigner.jar" verify --print-certs "$out\padreen.apk" | Select-String -Pattern 'certificate (DN|SHA-256)' | ForEach-Object { '  ' + $_.Line.Trim() }

if ($Install) {
    & $adb install -r "$out\padreen.apk"
    if ($LASTEXITCODE) { throw 'adb install failed' }
}
if ($Start) {
    & $adb shell am start -n "$pkg/.MainActivity"
}
