# Send a command line to the PaperProbe app and print the relevant logcat lines.
#   powershell -ExecutionPolicy Bypass -File probe.ps1 -Cmd "info" -WaitMs 1500
param(
    [Parameter(Mandatory)][string]$Cmd,
    [int]$WaitMs = 1500,
    [string]$Save = '',
    [switch]$NoClear,
    [string]$Pattern = 'PaperProbe|EinkSfpatchData|T1000EPDCDevice|hwtconepdcdisplay|t1000_lib|DISPLAY_DITHER|MD_DEBUG|MD_REFRESH|FBPostHwtcon|FBHand|AndroidRuntime|FATAL|hiddenapi|System.err'
)
$adb = 'D:\Android\android-sdk\platform-tools\adb.exe'
if (-not $NoClear) { & $adb logcat -c | Out-Null }
& $adb shell "am broadcast -a com.paperdisplay.probe.CMD -p com.paperdisplay.probe --es c '$Cmd'" | Out-Null
Start-Sleep -Milliseconds $WaitMs
$log = & $adb logcat -d -v threadtime
$sel = $log | Select-String -Pattern $Pattern | ForEach-Object { $_.Line }
if ($Save) { $sel | Set-Content -Encoding UTF8 $Save }
$sel
