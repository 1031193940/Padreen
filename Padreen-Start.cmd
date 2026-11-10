@echo off
rem Double-click launcher (prototype): tablet -> virtual monitor -> e-ink.  Close the window or press Ctrl-C to stop.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start-padreen.ps1" %*
pause
