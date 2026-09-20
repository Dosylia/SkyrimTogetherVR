@echo off
rem Applies an update zip. Drag the zip onto this file, or: update.bat "path\to\the-update.zip"
if "%~1"=="" (
    echo Drag the update zip onto this file, or run: update.bat "path\to\the-update.zip"
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update.ps1" -Zip "%~1" -ClientFolder "%~dp0."
pause
